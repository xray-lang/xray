/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Call displays receive actual argument provenance without matching labels. */
static bool source_call_display_range_equal(XrXirSourceRange a,XrXirSourceRange b) {
    return a.module==b.module&&a.line==b.line&&a.column==b.column&&
        a.end_line==b.end_line&&a.end_column==b.end_column;
}
static bool source_call_display_complete(SourceContext *ctx,AstNode *node,SourceCallPlan *plan) {
    if(!ctx->call_observations||!plan->display_syntax)return true;
    SourceCallSyntax original={0};if(!source_call_syntax(ctx,node,&original))return false;
    uint32_t count=(uint32_t)original.arg_count;
    uint32_t *formals=count?source_alloc(ctx,count,sizeof(*formals)):NULL;
    unsigned char *states=count?source_alloc(ctx,count,sizeof(*states)):NULL;
    if(count&&(!formals||!states))return false;
    for(uint32_t p=0;p<count;++p) {
        if(!(p%64)&&!source_request_poll(ctx,node))return false;
        formals[p]=UINT32_MAX;states[p]=0;
    }
    const SourceCallSyntax *expanded=plan->display_syntax;uint32_t source=0;
    uint32_t expanded_count=(uint32_t)expanded->arg_count;
    uint32_t *logical=expanded_count?source_alloc(ctx,expanded_count,sizeof(*logical)):NULL;
    if(expanded_count&&!logical)return false;
    XrXirStatus status=xr_hir_body_call_argument_formals(ctx->typed_body,plan->signature,ctx->request,logical,expanded_count);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"call display has no actual typed argument");
    for(uint32_t a=0;a<(uint32_t)expanded->arg_count;++a) {
        if(!(a%64)&&!source_request_poll(ctx,node))return false;
        AstNode *syntax=source_call_argument_syntax(expanded,a);
        while(source<count&&original.arguments[source]!=syntax) {
            if(!(source%64)&&!source_request_poll(ctx,node))return false;
            ++source;
        }
        if(source==count)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"call display lost original argument provenance");
        uint32_t formal=logical[a];bool known=formal!=UINT32_MAX;
        if(!known||states[source]==2||(states[source]&&formals[source]!=formal)) {
            formals[source]=UINT32_MAX;states[source]=2;
        } else {formals[source]=formal;states[source]=1;}
    }
    plan->display_argument_formals=formals;plan->display_argument_count=count;
    plan->display_argument_binding_known=true;
    if(plan->family!=SOURCE_CALL_NATIVE&&plan->family!=SOURCE_CALL_VARIADIC_READ&&
        plan->family!=SOURCE_CALL_NATIVE_CALLBACK)return true;
    XrXirSourceRange range=source_query_token_range(ctx,node);
    for(uint32_t c=ctx->calls.count;c;c--) {
        if(!(c%64)&&!source_request_poll(ctx,node))return false;
        XrXirSourceCall call=ctx->calls.items[c-1];
        if(!source_call_display_range_equal(call.range,range)||call.reference>=ctx->query.reference_count)continue;
        uint32_t target=ctx->query.references[call.reference].target;
        if(!target||target>ctx->query.declaration_count||!plan->declaration.native_member||
            ctx->query.declarations[target-1].native_identity!=plan->declaration.native_member->id)continue;
        XrXirSourceCall *destination=(XrXirSourceCall *)&ctx->calls.items[c-1];
        destination->argument_formals=formals;destination->argument_count=count;destination->argument_binding_known=true;
        break;
    }
    return true;
}
