/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_adoption.inc.c - Exact whole-root detachment and shared guard rebinding
 *
 * KEY CONCEPT:
 *   Terminal adoption neither allocates nor charges after successful preparation.
 */
static XrXirValueStatus view_ref_return_prepare_operation(const XrXirValue *root,
    const XrXirCellLoan *loan,const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    if (!admission_domain_current(admission) || !loan || !cell_authority_valid(authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell=cell_read_loan_root(root);
    if (!cell || cell->projection_count || cell->prepared_loan || cell->prepared_read_loan ||
        !admission_owner(admission,cell->object.arena,cell->object.domain) ||
        loan->cell!=cell || loan->scope!=cell || !cell_authority_equal(&loan->authority,authority))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!view_storage_work(admission,sizeof(*loan)+8u*sizeof(void *))) return XR_XIR_VALUE_LIMIT;
    const XrXirCellLoan *at=cell->borrow_top;
    while (at!=loan) {
        if (!view_storage_work(admission,1)) return XR_XIR_VALUE_LIMIT;
        if (!at || at->cell!=cell || at->scope!=cell || !at->previous || at->previous->child!=at)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        at=at->previous;
    }
    if ((loan->previous && loan->previous->child!=loan) ||
        (loan->child && loan->child->previous!=loan)) return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_view_ref_return_prepare(const XrXirValue *root,
    const XrXirCellLoan *loan,const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status=view_ref_return_prepare_operation(root,loan,authority,admission);
    xr_xir_value_graph_end();return status;
}
XR_FUNC void xr_xir_view_ref_return_commit(XrXirCellLoan *loan) {
    xr_xir_value_graph_begin();cell_loan_detach(loan);xr_xir_value_graph_end();
}
XR_FUNC void xr_xir_view_scope_adopt(XrXirViewOriginPin *pin,const XrXirCellAuthority *authority) {
    xr_xir_value_graph_begin();
    XR_CHECK(pin && pin->body && cell_authority_valid(&pin->authority) && cell_authority_valid(authority),
        "adoption owns a prepared returned leaf and a real receiving authority");
    XirView *view=pin->body;XrXirValue value=view_value_handle(view);
    XR_CHECK(xr_xir_value_valid(&value) && view_value_kind(view->object.kind) && view->children &&
        view->state==XIR_VIEW_ACTIVE,"a returned logical owner remains live through terminal adoption");
    if (view->storage.kind==XR_XIR_TYPE_SLICE) {
        XrXirCellReadLoan *read=&view->storage.loan.shared;
        XR_CHECK(read->previous && *read->previous==read && cell_authority_equal(&read->authority,&pin->authority),
            "shared adoption rebinds its exact authenticated guard before the old root ends");
        read->authority=*authority;pin->authority=*authority;
    }
    xr_xir_value_graph_end();
}
