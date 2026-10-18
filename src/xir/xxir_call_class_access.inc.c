/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_class_access.inc.c - Stack-owned access loans in the live frame
 */
XR_FUNC XrXirValueStatus xr_xir_call_class_access_prepare(const XrXirCallView *view,
    const XrXirValue *value,XrXirCallClassAccess *output) {
    if (!view || !value || !call_owner_current(view->activation) || !output || output->value ||
        (output->loan.cell || output->loan.scope || output->loan.previous ||
        output->loan.authority.activation || output->loan.authority.frame || output->loan.authority.epoch) || output->authority.activation ||
        output->authority.frame || output->authority.epoch) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCellAuthority authority={0};
    if (!view_cell_authority(view,&authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    /* Preparation, commit, abort/release and stack publication are prepaid.
     * No budget check is permitted after the exclusive loan is committed. */
    if (!call_cell_work(view->activation,sizeof(*output)+3*sizeof(output->loan)))
        return XR_XIR_VALUE_LIMIT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirCellLoan loan={0};
    XrXirValueStatus status=xr_xir_class_access_loan_prepare(value,&authority,admission,&loan);
    if (status==XR_XIR_VALUE_OK) *output=(XrXirCallClassAccess){value,authority,loan};
    return status;
}
XR_FUNC void xr_xir_call_class_access_commit(const XrXirCallView *view,XrXirCallClassAccess *access) {
    XrXirCellAuthority authority={0};
    XR_CHECK(access && access->value && view_cell_authority(view,&authority) &&
        authority.activation==access->authority.activation && authority.frame==access->authority.frame &&
        authority.epoch==access->authority.epoch,"immediate Class access retains its exact live frame");
    const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(access->value));
    if (xr_xir_type_is_cell(types,(XrXirType)access->value->type))
        xr_xir_cell_loan_commit(access->value,&access->loan,&authority);
    else xr_xir_class_loan_commit(access->value,&access->loan,&authority);
}
XR_FUNC void xr_xir_call_class_access_finish(XrXirCallClassAccess *access) {
    XR_CHECK(access && access->value && access->loan.cell,"immediate Class access owns one terminal loan");
    if (access->loan.authority.activation) xr_xir_cell_loan_release(&access->loan);
    else xr_xir_cell_loan_abort(&access->loan);
    *access=(XrXirCallClassAccess){0};
}
