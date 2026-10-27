/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_read_loan.inc.c - Shared root protection with allocation-free retirement
 *
 * KEY CONCEPT:
 *   Independent read lifetimes protect one root without imposing stack order.
 */
static bool cell_read_loan_empty(const XrXirCellReadLoan *loan) {
    return loan && !loan->cell && !loan->next && !loan->previous &&
        !loan->authority.activation && !loan->authority.frame && !loan->authority.epoch;
}
static XirCell *cell_read_loan_root(const XrXirValue *value) {
    if (!value_header_valid(value) || !owned_carrier_type((XrXirType)value->type)) return NULL;
    XirObject *object=object_pointer(value);
    if (object->kind!=XIR_OBJECT_CELL) return NULL;
    XirCell *cell=(XirCell *)object;
    return cell_root_header_valid(cell) ? cell : NULL;
}
XR_FUNC XrXirValueStatus xr_xir_cell_read_loan_prepare(const XrXirValue *value,
    const XrXirCellAuthority *authority,const XrXirCellReadLoan *parent,
    XrXirValueAdmission *admission,XrXirCellReadLoan *output) {
    if (!admission_domain_current(admission) || !cell_read_loan_empty(output) ||
        !cell_authority_valid(authority)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirCell *cell=cell_read_loan_root(value);
    if (!cell || cell->projection_count || cell->prepared_read_loan || cell->prepared_loan ||
        (parent ? parent->cell!=cell || !parent->previous || *parent->previous!=parent : !cell_access(cell,authority)) ||
        !admission_owner(admission,cell->object.arena,cell->object.domain)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission_is_compile(admission) && (admission->work<XR_XIR_CELL_READ_LOAN_WORK)) return XR_XIR_VALUE_LIMIT;
    if (!admission_is_compile(admission)) admission->work-=XR_XIR_CELL_READ_LOAN_WORK;
    if (!xr_xir_domain_is_compile(admission->domain) && (!xr_xir_domain_work(admission->domain,XR_XIR_CELL_READ_LOAN_WORK))) return XR_XIR_VALUE_LIMIT;
    *output=(XrXirCellReadLoan){.cell=cell,.authority=*authority};
    cell->prepared_read_loan=output;
    return XR_XIR_VALUE_OK;
}
XR_FUNC void xr_xir_cell_read_loan_abort(XrXirCellReadLoan *loan) {
    XR_CHECK(loan && loan->cell && !loan->previous && !loan->next,
        "only an unpublished read reservation may be aborted");
    XirCell *cell=loan->cell;
    XR_CHECK(cell->prepared_read_loan==loan,
        "read abort owns the exact prepared root reservation");
    cell->prepared_read_loan=NULL;
    *loan=(XrXirCellReadLoan){0};
}
XR_FUNC void xr_xir_cell_read_loan_commit(const XrXirValue *value,
    XrXirCellReadLoan *loan,const XrXirCellAuthority *authority,const XrXirCellReadLoan *parent) {
    XirCell *cell=cell_read_loan_root(value);
    XR_CHECK(cell && !cell->projection_count && loan && loan->cell==cell &&
        !loan->previous && !loan->next && cell->prepared_read_loan==loan &&
        cell_authority_equal(&loan->authority,authority) && !cell->prepared_loan &&
        (parent ? parent->cell==cell && parent->previous && *parent->previous==parent : cell_access(cell,authority)),
        "read commit preserves its actual root and live preparing authority");
    loan->next=cell->read_loans;
    loan->previous=&cell->read_loans;
    if (loan->next) loan->next->previous=&loan->next;
    cell->read_loans=loan;
    cell->prepared_read_loan=NULL;
}
XR_FUNC void xr_xir_cell_read_loan_release(XrXirCellReadLoan *loan) {
    XR_CHECK(loan && loan->cell,"read release retains its exact physical root");
    XirCell *cell=loan->cell;
    XR_CHECK(cell->prepared_read_loan!=loan,
        "a published read loan is not an unpublished reservation");
    if (!loan->previous) {
        XR_CHECK(!loan->next && !loan->authority.activation && !loan->authority.frame && !loan->authority.epoch,
            "an unlinked read reservation was explicitly revoked by its origin scope");
        *loan=(XrXirCellReadLoan){0};return;
    }
    XR_CHECK(*loan->previous==loan,"read release owns its exact published root link");
    *loan->previous=loan->next;
    if (loan->next) loan->next->previous=loan->previous;
    *loan=(XrXirCellReadLoan){0};
}
