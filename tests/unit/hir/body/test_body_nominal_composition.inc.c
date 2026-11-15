/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Supplier selection owns ordinals and leaves no partial state on failure. */
static void nominal_composition_receivers(XrHirBody *body,const XrHirNominalPool *pool,
    XrHirNominalPlan plan,XrHirNominalShape shape) {
    XrHirNominalInfo info={0};CHECK(xr_hir_body_nominal_info(body,shape,&info));
    XrHirNominalComponent components[]={
        {.shape=shape,.type=info.type,.value=2,.kind=XR_HIR_NOMINAL_SPREAD_VALUE},
        {.shape=shape,.type=info.type,.value=2,.kind=XR_HIR_NOMINAL_SPREAD_VALUE},
        {.name={"left",4},.type=XR_XIR_I64,.value=0,.kind=XR_HIR_NOMINAL_EXPLICIT_VALUE},
        {.name={"right",5},.type=XR_XIR_I64,.value=1,.kind=XR_HIR_NOMINAL_EXPLICIT_VALUE}};
    const XrHirNominalSupplier *out=(const XrHirNominalSupplier *)(uintptr_t)1;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(pool->compile->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t live=physical_live,bytes=physical_bytes;
    physical_fail=physical_attempts;
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,components,4,&out,NULL)==XR_XIR_OUT_OF_MEMORY);
    physical_fail=SIZE_MAX;
    CHECK(out==(const XrHirNominalSupplier *)(uintptr_t)1 && live==physical_live && bytes==physical_bytes);
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,components,2,&out,NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,components,3,&out,NULL)==XR_XIR_BAD_TYPE);
    XrHirNominalShape saved=components[0].shape;components[0].shape=(XrHirNominalShape){0};
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,components,4,&out,NULL)==XR_XIR_BAD_TYPE);
    components[0].shape=saved;
    CHECK(xr_compile_resources_stats(pool->compile->resources,&after)==XR_COMPILE_RESOURCE_OK &&
        after.held_bytes==before.held_bytes && live==physical_live && bytes==physical_bytes);
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,components,4,&out,NULL)==XR_XIR_OK);
    memset(components,0xa5,sizeof(components));
    CHECK(out[0].kind==XR_HIR_NOMINAL_EXPLICIT_VALUE && out[0].component==2 && out[0].value==0 && out[0].type==XR_XIR_I64 &&
        out[1].kind==XR_HIR_NOMINAL_EXPLICIT_VALUE && out[1].component==3 && out[1].value==1 && out[1].type==XR_XIR_I64);
    const XrHirNominalSupplier *untouched=NULL;
    CHECK(xr_hir_body_nominal_compose(body,plan,pool,NULL,0,&untouched,NULL)==XR_XIR_BAD_STRUCTURE && !untouched);
}
