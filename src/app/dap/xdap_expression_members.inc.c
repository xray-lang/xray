/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_expression_members.inc.c - Snapshot-authorized field inputs to ordinary Source
 */
#include "xdap_expression_member_tokens.inc.c"
/* Only consecutive nominal fields are observed here. Indexing, optional
 * unwraps, operators and intrinsic calls keep their ordinary Source semantics. */
static bool expression_member_project(ExpressionState *state,const AstNode *node) {
    const AstNode *fields[128],*root=node;uint32_t count=0,groups=0;
    while (root && (root->type==AST_MEMBER_ACCESS || root->type==AST_GROUPING)) {
        if (root->type==AST_GROUPING) { ++groups;root=root->as.grouping; }
        else {
            if (count==128) { state->status=XR_XIR_BUDGET;return false; }
            fields[count++]=root;root=root->as.member_access.object;
        }
        if (count+groups>=128-state->depth) { state->status=XR_XIR_BUDGET;return false; }
    }
    if (!root || root->type!=AST_VARIABLE || !count) return false;
    ExpressionBinding fresh={0};if (!expression_name_read(state,root->as.variable.name,&fresh)) return false;
    const XdapLexicalView *view=state->request->lexical;
    for (uint32_t i=count;i && state->status==XR_XIR_OK;--i) {
        const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(&fresh.value));
        if (!xr_xir_type_is_struct(types,(XrXirType)fresh.value.type) && !xr_xir_type_is_class(types,(XrXirType)fresh.value.type)) {
            xr_xir_value_drop(&fresh.value);return false;
        }
        if (!state->member_domain) state->status=expression_value_status(xr_xir_compile_domain_new(
            view->launch->resources,state->request->source_limit,&state->member_domain));
        if (state->status!=XR_XIR_OK) break;
        const char *name=fields[i-1]->as.member_access.name;
        if (!name) { state->status=XR_XIR_BAD_STRUCTURE;break; }
        XdapValueView observation={&fresh.value,state->member_domain,view->snapshot,view->frame};XrXirValue child={0};
        state->status=expression_value_status(xdap_value_member(&observation,name,strlen(name),&child));
        xr_xir_value_drop(&fresh.value);fresh.value=child;
    }
    uint32_t begin=0,end=0;
    if (state->status==XR_XIR_OK && expression_member_tokens(state)) {
        const MemberAccessNode *last=&node->as.member_access;
        if (expression_member_token(state,root->line,root->column,root->as.variable.name,&begin) &&
            expression_member_token(state,last->name_span.line,last->name_span.column,last->name,&end)) {
            for (uint32_t i=0;i<groups;++i) {
                if (!begin || state->tokens[begin-1].kind!=TK_LPAREN) { state->status=XR_XIR_BAD_STRUCTURE;break; }
                --begin;
            }
            if (end<begin) state->status=XR_XIR_BAD_STRUCTURE;
        }
    }
    if (state->status==XR_XIR_OK && state->count==128) state->status=XR_XIR_BUDGET;
    if (state->status==XR_XIR_OK && expression_member_name(state,&fresh)) {
        ExpressionProjection projection={node,state->tokens[begin].offset,
            state->tokens[end].offset+state->tokens[end].length,state->count};
        state->projections[state->projection_count++]=projection;
        if (fresh.value.type!=XR_XIR_BOOL && !xr_xir_type_is_integer((XrXirType)fresh.value.type)) state->runtime=true;
        state->bindings[state->count++]=fresh;return true;
    }
    xr_xir_value_drop(&fresh.value);if (fresh.owned_name) xr_compile_resources_free((void *)fresh.name);return false;
}
static XrXirStatus expression_members_rewrite(ExpressionState *state,XdapJsonWriter *text) {
    uint32_t order[128];
    for (uint32_t i=0;i<state->projection_count;++i) {
        uint32_t at=i;
        while (at) {
            if (state->projections[order[at-1]].begin<=state->projections[i].begin) break;
            order[at]=order[at-1];--at;
        }
        order[at]=i;
    }
    size_t offset=0;
    for (uint32_t i=0;i<state->projection_count;++i) {
        const ExpressionProjection *projection=&state->projections[order[i]];
        if (projection->begin<offset || projection->end<=projection->begin || projection->end>state->request->length)
            return XR_XIR_BAD_STRUCTURE;
        const ExpressionBinding *binding=&state->bindings[projection->binding];
        xdap_json_text(text,state->request->text+offset,projection->begin-offset);
        xdap_json_text(text,binding->name,binding->length);offset=projection->end;
    }
    xdap_json_text(text,state->request->text+offset,state->request->length-offset);return text->status;
}
