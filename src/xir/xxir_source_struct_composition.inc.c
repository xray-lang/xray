/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_struct_composition.inc.c - Written values consume HIR suppliers
 */
static bool source_struct_components(SourceContext *ctx,AstNode *node,SourceName *symbol,
    XrHirNominalShape shape,XrHirNominalPlan plan,char **names,AstNode **values,uint32_t count) {
    if(count && (!names || !values))return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"struct literal components are missing");
    XrHirNominalComponent *components=count?source_alloc(ctx,count,sizeof(*components)):NULL;
    if(count && !components)return false;
    for(uint32_t c=0;c<count;++c) {
        if(!source_request_poll(ctx,node))return false;
        AstNode *expression=values[c];SourceValue value={0};XrHirNominalComponent component={0};
        if(names[c]) {
            size_t length=source_text_size(ctx,names[c]);
            if(length>UINT32_MAX)return source_fail(ctx,node,XR_XIR_BUDGET,"field name size exhausted");
            uint32_t field=0;const char *reason=NULL;
            XrXirStatus status=xr_hir_body_nominal_find(ctx->typed_body,shape,(XrXirLiteral){names[c],(uint32_t)length},
                ctx->request,&field,&reason);
            if(!source_hir_nominal_status(ctx,expression,status,reason))return false;
            XrHirNominalField expected={0};
            if(!source_hir_nominal_field(ctx,expression,shape,field,&expected) ||
                !source_plan_expression(ctx,expression,(SourceExpectedType){expected.type!=XR_XIR_UNIT,expected.type,false,false},&value))return false;
            component.name=(XrXirLiteral){names[c],(uint32_t)length};component.kind=XR_HIR_NOMINAL_EXPLICIT_VALUE;
        } else {
            if(!expression || expression->type!=AST_SPREAD_EXPR || !expression->as.spread_expr.expr)
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"struct spread expression is missing");
            if(!source_plan_expression(ctx,expression->as.spread_expr.expr,(SourceExpectedType){0},&value))return false;
            const XrXirTypeNode *type=xr_xir_type_node(&ctx->types,value.type);
            if(type && type->kind==XR_XIR_TYPE_NOMINAL &&
                !source_hir_nominal_shape(ctx,expression,value.type,&component.shape,NULL))return false;
            component.kind=XR_HIR_NOMINAL_SPREAD_VALUE;
        }
        component.type=value.type;component.value=value.id;components[c]=component;
    }
    XrHirNominalPool pool=source_hir_nominal_pool(ctx);const XrHirNominalSupplier *suppliers=NULL;
    const char *reason=NULL;
    XrXirStatus status=xr_hir_body_nominal_compose(ctx->typed_body,plan,&pool,components,count,&suppliers,&reason);
    if(!source_hir_nominal_status(ctx,node,status,reason))return false;
    XrHirNominalInfo info={0};
    if(!xr_hir_body_nominal_info(ctx->typed_body,shape,&info))return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"struct supplier shape is missing");
    for(uint32_t f=0;f<info.count;++f) {
        XrHirNominalSupplier supplier=suppliers[f];
        if(supplier.kind==XR_HIR_NOMINAL_DEFAULT)continue;
        AstNode *site=values[supplier.component];XrHirNominalField field={0};uint32_t selected=0;
        if(!source_hir_nominal_field(ctx,site,shape,f,&field))return false;
        status=xr_hir_body_nominal_select(ctx->typed_body,plan,field.name,ctx->request,&selected,&reason);
        if(!source_hir_nominal_status(ctx,site,status,reason) || selected!=f)return false;
        SourceValue value={.id=supplier.value,.type=supplier.type};XrHirBodyUse producer={0};
        if(supplier.kind==XR_HIR_NOMINAL_SPREAD_VALUE) {
            XrHirNominalComponent source=components[supplier.component];
            if(!source_hir_nominal_get_use(ctx,site,(SourceValue){.id=source.value,.type=source.type},supplier.field,0,&value,&producer))return false;
        }
        status=xr_hir_body_nominal_supply(ctx->typed_body,plan,&pool,f,
            (XrHirNominalValue){.type=value.type,.value=value.id,.producer=producer},&reason);
        if(!source_hir_nominal_status(ctx,site,status,reason) ||
            !source_query_target_token_reference(ctx,site,ctx->nominal_members[symbol->index][f],XR_XIR_SOURCE_READ))return false;
    }
    source_retire_array(ctx,components);
    return true;
}
