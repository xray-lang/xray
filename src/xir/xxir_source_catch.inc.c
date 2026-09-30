/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_catch.inc.c - Ordered error regions, panic regions and scoped handler bindings
 *
 * KEY CONCEPT:
 *   A handler filters by one concrete enum type. Pattern handlers narrow the
 *   Error first and then reuse the typed match tree on the narrowed value, so
 *   a failed type, variant, literal or range test continues with the next
 *   handler and an unmatched error propagates unchanged. A panic handler
 *   protects exactly the blocks of its try body; nested bodies keep their own
 *   innermost handler, and no handler protects itself or its siblings.
 */

/* The first alternative names the handler's enum type. An Error carries no
 * static type arguments, so a generic enum states them there; the remaining
 * alternatives are checked against that type exactly as match patterns are
 * checked against a scrutinee. */
static bool source_catch_pattern_type(SourceContext *ctx, AstNode *pattern, XrXirType *type) {
    AstNode *path = pattern->type == AST_PATTERN_ADT ? pattern->as.pattern_adt.variant :
        pattern->type == AST_PATTERN_LITERAL ? pattern->as.pattern_literal.value : NULL;
    if (!path || path->type != AST_MEMBER_ACCESS || !path->as.member_access.object)
        return source_fail(ctx, pattern, XR_XIR_BAD_TYPE, "catch pattern must name an enum variant");
    SourceTypeArguments arguments = {0}; SourceName *owner = NULL, *binding = NULL;
    if (!source_nominal_path(ctx, path->as.member_access.object, &arguments, &binding, &owner)) return false;
    if (!owner || owner->kind != SOURCE_NOMINAL ||
        ctx->nominals.declarations[owner->index].kind != XR_XIR_NOMINAL_ENUM)
        return source_fail(ctx, pattern, XR_XIR_BAD_TYPE, "catch pattern must name a visible enum variant");
    if (arguments.count != ctx->nominals.declarations[owner->index].parameter_count)
        return source_fail(ctx, pattern, XR_XIR_BAD_TYPE, "catch pattern must state the enum's type arguments");
    return source_nominal_apply(ctx, owner, arguments.refs, arguments.count, type);
}
static bool source_catch_pattern(SourceContext *ctx, AstNode *root, XrXirType *type, SourceMatchArm *arm) {
    uint32_t count = 1;
    AstNode **alternatives = &root;
    if (root->type == AST_PATTERN_MULTI) {
        if (root->as.pattern_multi.count <= 0 || !root->as.pattern_multi.patterns)
            return source_fail(ctx, root, XR_XIR_BAD_STRUCTURE, "catch pattern requires alternatives");
        count = (uint32_t)root->as.pattern_multi.count;
        alternatives = root->as.pattern_multi.patterns;
    }
    if (!alternatives[0] || !source_catch_pattern_type(ctx, alternatives[0], type)) return false;
    arm->count = count; arm->bindings = NULL;
    arm->patterns = source_alloc(ctx, count, sizeof(*arm->patterns));
    if (!arm->patterns) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!alternatives[i] || !source_match_pattern(ctx, alternatives[i], *type, &arm->patterns[i], 0) ||
            !source_pattern_bindings(ctx, &arm->patterns[i], arm, i, 0)) return false;
        for (SourceName *symbol = arm->bindings; symbol; symbol = symbol->next) {
            if (!source_work(ctx, alternatives[i])) return false;
            if (symbol->index != i + 1)
                return source_fail(ctx, alternatives[i], XR_XIR_BAD_TYPE, "alternative is missing a common binding");
        }
    }
    return true;
}
/* A handler without an error predecessor is checked but never executed: its
 * pattern bindings name a placeholder that no published graph can reference. */
static bool source_catch_placeholders(SourceContext *ctx, const SourceMatchArm *arm) {
    for (SourceName *symbol = arm->bindings; symbol; symbol = symbol->next)
        if (!source_work(ctx, symbol->node) ||
            !source_match_bind(ctx, symbol, (SourceValue){UINT32_MAX, symbol->type})) return false;
    return true;
}
static bool source_catch_body(SourceContext *ctx, XrCatchClause *clause, SourceMatchArm *arm,
    SourceValue error, SourceMatchFailure **failures) {
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    if (clause->var_name) {
        SourceName *binding = source_alloc(ctx,1,sizeof(*binding));
        if (!binding) return false;
        *binding = (SourceName){locals,clause->var_name,NULL,clause->body,SOURCE_LOCAL,
            error.id,ctx->module,error.type,false,false,0};
        ctx->locals = binding;
        XrXirSourceRange range = {ctx->module,clause->var_line,clause->var_column,
            clause->var_line,clause->var_column+(uint32_t)strlen(clause->var_name)};
        if (!source_query_declare(ctx,binding,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,range)) return false;
        source_query_binding_type(ctx,binding);
    } else if (clause->pattern) {
        bool bound = error.id == UINT32_MAX ? source_catch_placeholders(ctx,arm) :
            source_match_alternatives(ctx,arm,error,true,failures);
        if (!bound) return false;
    }
    bool ok = scoped_statement(ctx,clause->body);
    ctx->locals = locals; ctx->scope = scope; return ok;
}
static bool source_catch_snapshot(SourceContext *ctx, XrXirInitializationRegion *region) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    XrXirInstruction *ops=source_alloc(ctx,body->count,sizeof(*ops));
    XrXirBlock *blocks=source_alloc(ctx,body->block_count,sizeof(*blocks));
    uint32_t *operands=body->operand_count ? source_alloc(ctx,body->operand_count,sizeof(*operands)) : NULL;
    if (!ops || !blocks || (body->operand_count && !operands)) return false;
    memcpy(ops,body->ops,body->count*sizeof(*ops));
    memcpy(blocks,body->blocks,body->block_count*sizeof(*blocks));
    if (body->operand_count) memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
    region->function=ctx->functions[ctx->function];
    region->function.instructions=ops; region->function.instruction_count=body->count;
    region->function.blocks=blocks; region->function.block_count=body->block_count;
    region->function.operands=operands; region->function.operand_count=body->operand_count;
    region->next=body->initialization_regions; body->initialization_regions=region;
    return true;
}
static bool source_dead_catch(SourceContext *ctx, XrCatchClause *clause, SourceMatchArm *arm,
    XrXirType type, uint32_t checkpoint) {
    SourceFunction *body = &ctx->bodies[ctx->function], saved = *body;
    XrXirGeneric generic = ctx->generics[ctx->function];
    bool returned = ctx->returned;
    uint32_t count = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) ++count;
    SourceLoop *loops = count ? source_alloc(ctx,count,sizeof(*loops)) : NULL;
    if (count && !loops) return false;
    uint32_t i = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) loops[i++] = *loop;
    XrXirInitializationRegion *region=NULL;
    if (source_constructor_active(ctx)) {
        region=source_alloc(ctx,1,sizeof(*region));
        if (!region) return false;
        region->parent=body->initialization_parent; region->checkpoint=checkpoint;
        region->first_instruction=body->count; region->first_block=body->block_count;
        body->initialization_parent=region;
    }
    body->error_context = NULL;
    bool ok = begin_block(ctx) && source_catch_body(ctx,clause,arm,(SourceValue){UINT32_MAX,type},NULL);
    if (ok && region) ok=source_catch_snapshot(ctx,region);
    i = 0;
    for (SourceLoop *loop = ctx->loop; loop; loop = loop->parent) *loop = loops[i++];
    saved.saw_return = body->saw_return;
    saved.initialization_regions=body->initialization_regions;
    *body = saved; ctx->generics[ctx->function] = generic; ctx->returned = returned;
    return ok;
}
static bool source_catch_join(SourceContext *ctx, uint32_t *joins, uint32_t *count) {
    if (ctx->returned) return true;
    joins[(*count)++] = ctx->bodies[ctx->function].count;
    return emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL);
}
static bool source_catch_dispatch(SourceContext *ctx, TryCatchNode *attempt, const XrXirType *types,
    SourceMatchArm *patterns, SourceValue error, uint32_t checkpoint, uint32_t *joins, uint32_t *count) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    bool consumed = false;
    for (int i = 0; i < attempt->catch_count; ++i) {
        XrCatchClause *clause = attempt->catch_clauses[i];
        if (clause->is_panic) continue;
        if (!source_work(ctx,clause->body)) return false;
        if (consumed) {
            if (!source_dead_catch(ctx,clause,&patterns[i],types[i],checkpoint)) return false;
            continue;
        }
        SourceValue bound = error;
        SourceMatchFailure *failures = NULL;
        uint32_t branch = UINT32_MAX;
        if (types[i] != XR_XIR_ERROR) {
            SourceValue test;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_ERROR_IS,XR_XIR_BOOL,{error.id},{0},types[i],{0}},&test)) return false;
            branch = body->count;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{test.id},{0},0,{0}},NULL)) return false;
            body->ops[branch].targets[0] = body->block_count;
            if (!begin_block(ctx) ||
                !emit(ctx,(XrXirInstruction){XR_XIR_ERROR_NARROW,types[i],{error.id},{0},0,{0}},&bound)) return false;
        } else consumed = true;
        if (!source_catch_body(ctx,clause,&patterns[i],bound,&failures) || !source_catch_join(ctx,joins,count)) return false;
        if (branch != UINT32_MAX) {
            uint32_t next = body->block_count;
            body->ops[branch].targets[1] = next;
            for (SourceMatchFailure *failure = failures; failure; failure = failure->next) {
                if (!source_work(ctx,clause->body)) return false;
                body->ops[failure->instruction].targets[failure->target] = next;
            }
            if (!begin_block(ctx)) return false;
        }
    }
    if (!consumed) {
        if (body->error_context) {
            if (!source_error_edge(ctx,error)) return false;
        } else if (!emit(ctx,(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{error.id},{0},0,{0}},NULL)) return false;
    }
    return true;
}
/* The handler block begins with its selector; the body's blocks are patched
 * afterwards, so blocks already owned by a nested handler keep it. */
static bool source_panic_handler(SourceContext *ctx, XrCatchClause *clause, uint32_t first, uint32_t end,
    uint32_t *joins, uint32_t *count) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t handler = body->block_count;
    SourceValue caught = {0, XR_XIR_UNIT};
    if (!begin_block(ctx) || !emit(ctx,(XrXirInstruction){XR_XIR_PANIC_CATCH,
            clause->var_name ? XR_XIR_PANIC_INFO : XR_XIR_UNIT,{0},{0},0,{0}},&caught) ||
        !source_catch_body(ctx,clause,NULL,caught,NULL) || !source_catch_join(ctx,joins,count)) return false;
    for (uint32_t b = first; b < end; ++b) {
        if (!source_work(ctx,clause->body)) return false;
        if (!body->blocks[b].panic) body->blocks[b].panic = handler;
    }
    return true;
}
static XrXirStatus source_catch_type_proof(SourceContext *ctx, XrXirType type) {
    XrXirDeclarations declarations;
    XrXirModule view = source_module_view(ctx,&declarations);
    XrXirProofContext context = {&view,{XR_XIR_CONTEXT_FUNCTION,ctx->function}};
    XrXirStatus status = xr_xir_type_markers_prove(&context,type,XR_XIR_CONSTRAINT_ERROR,&ctx->budget);
    return status == XR_XIR_OK ? xr_xir_type_access(&view,ctx->function,type,&ctx->budget) : status;
}
static bool source_try(SourceContext *ctx, AstNode *node) {
    TryCatchNode *attempt = &node->as.try_catch;
    if (attempt->catch_count < 1 || !attempt->catch_clauses)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"try requires error handlers");
    XrXirType *types = source_alloc(ctx,attempt->catch_count,sizeof(*types));
    SourceMatchArm *patterns = source_alloc(ctx,attempt->catch_count,sizeof(*patterns));
    uint32_t *joins = source_alloc(ctx,(size_t)attempt->catch_count+1,sizeof(*joins)), count = 0;
    if (!types || !patterns || !joins) return false;
    XrCatchClause *panic = NULL;
    bool ordinary = false;
    for (int i = 0; i < attempt->catch_count; ++i) {
        XrCatchClause *clause = attempt->catch_clauses[i]; types[i] = XR_XIR_ERROR;
        if (!source_work(ctx,node)) return false;
        if (!clause || !clause->body || (clause->pattern && (clause->var_name || clause->is_panic)))
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"malformed catch clause");
        if (clause->is_panic) {
            if (panic) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"try allows one catch panic clause");
            if (clause->type && (!source_type(ctx,clause->type,&types[i]) || types[i] != XR_XIR_PANIC_INFO))
                return source_fail(ctx,node,XR_XIR_BAD_TYPE,"catch panic binding must have type PanicInfo");
            types[i] = XR_XIR_PANIC_INFO; panic = clause;
            continue;
        }
        ordinary = true;
        /* A pattern's own path names its enum; the parser's head annotation is
         * only an analyzer hint and is never read here. */
        if (clause->pattern) {
            if (!source_catch_pattern(ctx,clause->pattern,&types[i],&patterns[i])) return false;
        } else if (clause->type && (!source_type(ctx,clause->type,&types[i]) || !xr_xir_type_is_enum(&ctx->types,types[i])))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"typed catch requires a concrete enum type");
        if (types[i] != XR_XIR_ERROR) {
            XrXirStatus status = source_catch_type_proof(ctx,types[i]);
            if (status != XR_XIR_OK) return source_fail(ctx,node,status,"catch target lacks its definition-time type proof");
        }
    }
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t checkpoint=body->count, first = 0;
    if (panic) {
        if (!body->block_count && !begin_block(ctx)) return false;
        first = body->block_count;
        if (!emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{first,0},0,{0}},NULL) || !begin_block(ctx)) return false;
    }
    SourceErrorContext handler = {0}, *outer = body->error_context;
    handler.frontier = body->frontier;
    if (ordinary) body->error_context = &handler;
    bool ok = scoped_statement(ctx,attempt->try_body);
    body->error_context = outer;
    if (!ok) return false;
    uint32_t end = body->block_count;
    if (!handler.count && !panic) {
        for (int i = 0; i < attempt->catch_count; ++i)
            if (!source_dead_catch(ctx,attempt->catch_clauses[i],&patterns[i],types[i],checkpoint)) return false;
        return true;
    }
    if (!source_catch_join(ctx,joins,&count)) return false;
    if (handler.count) {
        uint32_t target = body->block_count;
        if (!begin_block(ctx)) return false;
        SourceValue *inputs = source_alloc(ctx,(size_t)handler.count*2,sizeof(*inputs));
        if (!inputs) return false;
        uint32_t at = handler.count;
        for (SourceErrorEdge *edge = handler.edges; edge; edge = edge->next) {
            if (!source_work(ctx,node)) return false;
            body->ops[edge->jump].targets[0] = target;
            --at; inputs[at*2] = (SourceValue){edge->block,XR_XIR_UNIT}; inputs[at*2+1] = edge->value;
        }
        SourceValue error;
        if (!emit_group(ctx,(XrXirInstruction){XR_XIR_PHI,XR_XIR_ERROR,{0},{0},0,{0}},inputs,handler.count*2,&error) ||
            !source_catch_dispatch(ctx,attempt,types,patterns,error,checkpoint,joins,&count)) return false;
    } else for (int i = 0; i < attempt->catch_count; ++i)
        if (!attempt->catch_clauses[i]->is_panic &&
            !source_dead_catch(ctx,attempt->catch_clauses[i],&patterns[i],types[i],checkpoint)) return false;
    if (panic && !source_panic_handler(ctx,panic,first,end,joins,&count)) return false;
    ctx->returned = count == 0;
    if (!count) return true;
    uint32_t target = body->block_count;
    if (!begin_block(ctx)) return false;
    for (uint32_t i = 0; i < count; ++i) body->ops[joins[i]].targets[0] = target;
    return true;
}
