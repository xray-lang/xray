/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_match.inc.c - Ordered enum tests and owned arm bindings
 */
typedef struct SourceMatchPattern {
    AstNode *node, *binding;
    PatternAdtNode *payload;
    SourceEnumSelection selection;
    bool any;
} SourceMatchPattern;
static bool source_pattern_binding(AstNode *node) {
    return node && node->type == AST_PATTERN_LITERAL && node->as.pattern_literal.value &&
        node->as.pattern_literal.value->type == AST_VARIABLE;
}
static bool source_match_pattern(SourceContext *ctx, AstNode *node, XrXirType type, SourceMatchPattern *out) {
    out->node = node;
    if (node->type == AST_PATTERN_WILDCARD || source_pattern_binding(node)) {
        out->any = true;
        if (source_pattern_binding(node)) out->binding = node->as.pattern_literal.value;
        return true;
    }
    AstNode *path = NULL;
    if (node->type == AST_PATTERN_ADT) { out->payload = &node->as.pattern_adt; path = out->payload->variant; }
    else if (node->type == AST_PATTERN_LITERAL) path = node->as.pattern_literal.value;
    if (!path || path->type != AST_MEMBER_ACCESS)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"match pattern family is not admitted");
    SourceTypeArguments arguments = {0}; SourceName *owner = NULL, *binding = NULL;
    if (!source_nominal_path(ctx,path->as.member_access.object,&arguments,&binding,&owner)) return false;
    uint32_t declaration = xr_xir_type_node(&ctx->types,type)->nominal.declaration;
    if (!owner || owner->kind != SOURCE_NOMINAL || owner->index != declaration)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern must name the scrutinee enum declaration");
    XrXirType actual = type;
    if (arguments.count && (!source_nominal_apply(ctx,owner,arguments.refs,arguments.count,&actual) || actual != type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern type arguments differ from the scrutinee");
    const XrXirNominalDeclaration *d = &ctx->nominals.declarations[declaration];
    uint32_t v = 0;
    for (; v < d->variant_count; ++v) {
        if (!source_work(ctx,node)) return false;
        if (strlen(path->as.member_access.name)==d->variants[v].name.length &&
            !memcmp(path->as.member_access.name,d->variants[v].name.bytes,d->variants[v].name.length)) break;
    }
    if (v == d->variant_count || (out->payload != NULL) != (d->variants[v].field_count != 0))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pattern variant or payload syntax mismatch");
    if (out->payload) {
        if (out->payload->count < 0) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"invalid pattern field count");
        for (int f=0;f<out->payload->count;++f) {
            AstNode *sub=out->payload->patterns[f];
            if (!source_work(ctx,sub)) return false;
            if (sub->type != AST_PATTERN_WILDCARD && !source_pattern_binding(sub))
                return source_fail(ctx,sub,XR_XIR_BAD_TYPE,"payload subpattern is not admitted");
        }
    }
    out->selection=(SourceEnumSelection){owner,binding,path->as.member_access.object,type,v};
    return source_query_reference(ctx,out->selection.path,binding,owner,XR_XIR_SOURCE_TYPE_USE) &&
        source_query_target_reference(ctx,source_query_range(ctx,path,NULL),ctx->nominal_variants[declaration][v],XR_XIR_SOURCE_READ);
}
static bool source_match_bind(SourceContext *ctx, AstNode *node, SourceValue value) {
    for (SourceName *p=ctx->locals;p!=ctx->scope;p=p->next) {
        if (!source_work(ctx,node)) return false;
        if (!strcmp(p->name,node->as.variable.name)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"duplicate pattern binding");
    }
    SourceName *binding=source_alloc(ctx,1,sizeof(*binding));
    if (!binding) return false;
    *binding=(SourceName){ctx->locals,node->as.variable.name,NULL,node,SOURCE_LOCAL,value.id,ctx->module,value.type,false,false,0};
    ctx->locals=binding;
    if (!source_query_declare(ctx,binding,XR_XIR_SOURCE_BINDING,ctx->bodies[ctx->function].declaration,
        source_query_range(ctx,node,binding->name))) return false;
    source_query_binding_type(ctx,binding); return true;
}
static bool source_match_payload(SourceContext *ctx, SourceMatchPattern *pattern, SourceValue receiver) {
    if (pattern->binding) return source_match_bind(ctx,pattern->binding,receiver);
    if (!pattern->payload) return true;
    SourceEnumSelection *selected=&pattern->selection;
    const XrXirNominalDeclaration *d=&ctx->nominals.declarations[selected->owner->index];
    XrXirNominalVariant variant=d->variants[selected->variant];
    const XrXirNominalType *nominal=&xr_xir_type_node(&ctx->types,receiver.type)->nominal;
    SourceSubstitution substitution={nominal->arguments,nominal->argument_count};
    bool *seen=source_alloc(ctx,variant.field_count,sizeof(*seen));
    if (!seen) return false;
    for (int i=0;i<pattern->payload->count;++i) {
        const char *name=pattern->payload->field_names[i]; uint32_t field=0;
        for (;field<variant.field_count;++field) {
            if (!source_work(ctx,pattern->node)) return false;
            XrXirLiteral spelling=d->fields[variant.field_begin+field].name;
            if (strlen(name)==spelling.length && !memcmp(name,spelling.bytes,spelling.length)) break;
        }
        if (field==variant.field_count || seen[field]) return source_fail(ctx,pattern->node,XR_XIR_BAD_TYPE,"unknown or duplicate pattern field");
        seen[field]=true;
        XrNameSpan span=pattern->payload->field_name_spans[i];
        XrXirSourceRange range={ctx->module,span.line,span.column,span.line,span.column};
        if (span.column>0 && strlen(name)<=(size_t)(INT_MAX-span.column)) range.end_column+=(int)strlen(name);
        if (!source_query_target_reference(ctx,range,ctx->nominal_members[selected->owner->index][variant.field_begin+field],XR_XIR_SOURCE_READ)) return false;
        AstNode *sub=pattern->payload->patterns[i];
        if (sub->type==AST_PATTERN_WILDCARD) continue;
        XrXirType type; SourceValue value;
        if (!source_substitute(ctx,&substitution,d->fields[variant.field_begin+field].type,0,&type) ||
            !emit(ctx,(XrXirInstruction){XR_XIR_ENUM_GET,type,{receiver.id,field},{0},selected->variant},&value) ||
            !source_match_bind(ctx,sub->as.pattern_literal.value,value)) return false;
    }
    return true;
}
static bool source_match_body(SourceContext *ctx, AstNode *node, XrXirType expected, SourceValue *value) {
    if (node->type != AST_BLOCK) return expression_in(ctx,node,expected,value);
    *value=(SourceValue){0,XR_XIR_UNIT};
    for (int i=0;i<node->as.block.count;++i) {
        AstNode *part=node->as.block.statements[i];
        if (i+1==node->as.block.count && part->type==AST_EXPR_STMT)
            return expression_in(ctx,part->as.expr_stmt,expected,value);
        if (!statement(ctx,part,false)) return false;
        if (ctx->returned) {
            if (i+1!=node->as.block.count) return source_fail(ctx,part,XR_XIR_BAD_STRUCTURE,"unreachable match statements are not admitted");
            return true;
        }
    }
    return true;
}
static bool source_match(SourceContext *ctx, AstNode *node, XrXirType expected, bool result_required, SourceValue *value) {
    MatchExprNode *match=&node->as.match_expr; SourceValue receiver,tag;
    if (match->arm_count<=0 || (uint32_t)match->arm_count>UINT32_MAX/2 || !expression(ctx,match->expr,&receiver))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"match requires arms and a scrutinee");
    if (!xr_xir_type_is_enum(&ctx->types,receiver.type)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"match scrutinee family is not admitted");
    uint32_t declaration=xr_xir_type_node(&ctx->types,receiver.type)->nominal.declaration;
    uint32_t variants=ctx->nominals.declarations[declaration].variant_count, count=(uint32_t)match->arm_count;
    SourceMatchPattern *patterns=source_alloc(ctx,count,sizeof(*patterns));
    bool *covered=source_alloc(ctx,variants,sizeof(*covered));
    SourceValue *inputs=source_alloc(ctx,count*2,sizeof(*inputs)); uint32_t *jumps=source_alloc(ctx,count,sizeof(*jumps));
    if (!patterns || !covered || !inputs || !jumps) return false;
    uint32_t coverage=0;
    for (uint32_t i=0;i<count;++i) {
        AstNode *arm=match->arms[i];
        if (!source_work(ctx,arm) || arm->type!=AST_MATCH_ARM || !source_match_pattern(ctx,arm->as.match_arm.pattern,receiver.type,&patterns[i])) return false;
        if (coverage==variants) return source_fail(ctx,arm,XR_XIR_BAD_STRUCTURE,"unreachable match arm is not admitted");
        if (!arm->as.match_arm.guard) {
            if (patterns[i].any) coverage=variants;
            else if (!covered[patterns[i].selection.variant]) { covered[patterns[i].selection.variant]=true; ++coverage; }
        }
    }
    if (coverage!=variants) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"enum match is not exhaustive");
    if (!emit(ctx,(XrXirInstruction){XR_XIR_ENUM_TAG,XR_XIR_I64,{receiver.id,0},{0},0},&tag)) return false;
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceName *saved=ctx->locals,*scope=ctx->scope; XrXirType result=XR_XIR_UNIT;
    uint32_t continuing=0;
    for (uint32_t i=0;i<count;++i) {
        MatchArmNode *arm=&match->arms[i]->as.match_arm; uint32_t branch=UINT32_MAX,guard=UINT32_MAX;
        ctx->locals=saved; ctx->scope=saved;
        if (i+1<count && !patterns[i].any) {
            SourceValue ordinal,condition;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},patterns[i].selection.variant},&ordinal) ||
                !emit(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{tag.id,ordinal.id},{0},0},&condition)) return false;
            branch=body->count; uint32_t yes=body->block_count;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id,0},{yes,0},0},NULL) || !begin_block(ctx)) return false;
        }
        if (!source_match_payload(ctx,&patterns[i],receiver)) return false;
        if (arm->guard) {
            SourceValue condition;
            if (!expression(ctx,arm->guard,&condition)) return false;
            if (condition.type!=XR_XIR_BOOL) return source_fail(ctx,arm->guard,XR_XIR_BAD_TYPE,"match guard requires bool");
            guard=body->count; uint32_t yes=body->block_count;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id,0},{yes,0},0},NULL) || !begin_block(ctx)) return false;
        }
        SourceValue output;
        if (!source_match_body(ctx,arm->body,expected,&output)) return false;
        if (!ctx->returned) {
            if (!continuing) result=output.type;
            else if (result!=output.type) return source_fail(ctx,arm->body,XR_XIR_BAD_TYPE,"match arms need the same admitted type");
            inputs[continuing*2]=(SourceValue){body->block_count-1,XR_XIR_UNIT};
            inputs[continuing*2+1]=output; jumps[continuing++]=body->count;
            if (!emit(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0},NULL)) return false;
        }
        uint32_t next=body->block_count;
        if (branch!=UINT32_MAX) body->ops[branch].targets[1]=next;
        if (guard!=UINT32_MAX) body->ops[guard].targets[1]=next;
        if (i+1<count && !begin_block(ctx)) return false;
    }
    ctx->locals=saved; ctx->scope=scope;
    if (!continuing) {
        if (result_required) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"never-valued expression context is not admitted");
        ctx->returned=true; *value=(SourceValue){0,XR_XIR_UNIT}; return true;
    }
    uint32_t join=body->block_count;
    if (!begin_block(ctx)) return false;
    for (uint32_t i=0;i<continuing;++i) body->ops[jumps[i]].targets[0]=join;
    if (result==XR_XIR_UNIT) { *value=(SourceValue){0,XR_XIR_UNIT}; return true; }
    return emit_group(ctx,(XrXirInstruction){XR_XIR_PHI,result,{0},{0},0},inputs,continuing*2,value);
}
