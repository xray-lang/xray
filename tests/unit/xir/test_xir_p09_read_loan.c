/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_read_loan.c - Shared root reads exclude every existing mutable route
 *
 * KEY CONCEPT:
 *   Independent read lifetimes end without stack order or changing the root.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_read_loan_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY

static XrXirTypeArena *read_loan_arena(void) {
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING}
    };
    const XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(1048576,1048576,1000000),&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static XrXirValueStatus read_loan_domain(XrXirDomain **domain) {
    const XrXirDomainBudgetControls controls={65536,65536,1000000,65536,65536};
    return xr_xir_domain_new_budgeted(65536,&controls,domain);
}
static void read_loan_read(const XrXirValue *cell,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,int64_t expected) {
    const XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)256,0};
    const XrXirValuePath path={&step,1};XrXirValue value={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_cell_authorized_path_read(cell,authority,&path,admission,&value,&fault)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==expected && !fault.code);xr_xir_value_drop(&value);
}
static void read_loan_mutation_denied(const XrXirValue *cell,const XrXirValue *projection,
    const XrXirCellAuthority *authority,XrXirValueAdmission *admission) {
    XrXirValue copy={0},number={XR_XIR_I64,0,99};XrXirValuePlace place={0};
    const XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)256,0};
    const XrXirValuePath path={&step,1},empty={0};XrXirFaultDetail fault={0};
    XrXirCellLoan exclusive={0};XrXirCellPublication publication={cell,admission->domain,0};
    CHECK(xr_xir_cell_read(cell,&copy)==XR_XIR_VALUE_OK && copy.type==256);
    CHECK(xr_xir_cell_write(cell,&copy,admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_write(cell,authority,&copy,admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_value_place(cell,admission,&place)==XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    CHECK(xr_xir_cell_authorized_place(cell,authority,admission,&place)==XR_XIR_VALUE_BAD_ARGUMENT && !place.payload);
    CHECK(xr_xir_cell_authorized_path_write(cell,authority,&path,&number,admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_path_push(cell,authority,&empty,&number,admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_write(projection,authority,&number,admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_path_write(projection,authority,&empty,&number,admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_loan_prepare(cell,authority,&exclusive)==XR_XIR_VALUE_BAD_ARGUMENT && !exclusive.cell);
    CHECK(xr_xir_cell_loan_prepare_path(projection,authority,admission,&fault,&exclusive)==XR_XIR_VALUE_BAD_ARGUMENT && !exclusive.cell);
    CHECK(xr_xir_cell_publication_prepare(cell,&publication)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xr_xir_cell_unborrowed(cell));
    read_loan_read(cell,authority,admission,11);xr_xir_value_drop(&copy);
}
static XrXirValueStatus read_loan_sequence(XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL;XrXirValue array={0},cell={0},projection={0};
    XrXirValueStatus status=read_loan_domain(&domain);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}};
    const XrXirCellAuthority authority={domain,arena,1};
    const XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)256,0};
    const XrXirValuePath path={&step,1},empty={0};XrXirFaultDetail fault={0};
    TRY_VALUE(xr_xir_array_new((XrXirType)256,items,2,&admission,&array));
    TRY_VALUE(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell));
    TRY_VALUE(xr_xir_cell_project_authorized((XrXirType)258,&cell,&path,&authority,&admission,&projection));
    XrXirCellReadLoan loans[3]={{0}},waiting={0};
    uint64_t work=xr_xir_domain_budget_stats(domain).work;size_t first=calls;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&waiting)==XR_XIR_VALUE_OK);
    uint64_t paid=xr_xir_domain_budget_stats(domain).work-work;
    CHECK(paid==XR_XIR_CELL_READ_LOAN_WORK);
    read_loan_mutation_denied(&cell,&projection,&authority,&admission);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loans[0])==XR_XIR_VALUE_BAD_ARGUMENT && !loans[0].cell);
    xr_xir_cell_read_loan_abort(&waiting);
    CHECK(!waiting.cell && xr_xir_cell_unborrowed(&cell));
    admission.work=paid-1;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&waiting)==XR_XIR_VALUE_LIMIT && !waiting.cell);
    admission.work=paid;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loans[0])==XR_XIR_VALUE_OK && !admission.work);
    xr_xir_cell_read_loan_commit(&cell,&loans[0],&authority);
    admission.work=1000000;
    for (unsigned i=1;i<3;++i) {
        CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loans[i])==XR_XIR_VALUE_OK);
        xr_xir_cell_read_loan_commit(&cell,&loans[i],&authority);
    }
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loans[0])==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_read_loan_release(&loans[0]);
    read_loan_mutation_denied(&cell,&projection,&authority,&admission);
    xr_xir_cell_read_loan_release(&loans[2]);
    read_loan_mutation_denied(&cell,&projection,&authority,&admission);
    xr_xir_cell_read_loan_release(&loans[1]);
    CHECK(xr_xir_cell_unborrowed(&cell) && calls==first);
    XrXirCellPublication publication={&cell,domain,0};
    CHECK(xr_xir_cell_publication_prepare(&cell,&publication)==XR_XIR_VALUE_OK);
    XrXirValue number={XR_XIR_I64,0,77};
    TRY_VALUE(xr_xir_cell_authorized_path_write(&cell,&authority,&path,&number,&admission,&fault));
    read_loan_read(&cell,&authority,&admission,77);
    number.payload=88;
    TRY_VALUE(xr_xir_cell_authorized_write(&projection,&authority,&number,&admission));
    read_loan_read(&cell,&authority,&admission,88);
    TRY_VALUE(xr_xir_cell_authorized_path_push(&cell,&authority,&empty,&number,&admission,&fault));
    XrXirValuePlace place={0};TRY_VALUE(xr_xir_cell_authorized_place(&cell,&authority,&admission,&place));
    CHECK(place.type==256 && place.payload);
    XrXirValue original={0};CHECK(xr_xir_array_get(&array,0,&admission,&original,&fault)==XR_XIR_VALUE_OK);
    CHECK(original.type==XR_XIR_I64 && original.payload==11);xr_xir_value_drop(&original);
done:
    xr_xir_value_drop(&projection);xr_xir_value_drop(&cell);xr_xir_value_drop(&array);
    xr_xir_domain_drop(domain);CHECK(!live);return status;
}
static void read_loan_authority(XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL;CHECK(read_loan_domain(&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    XrXirValue scalar={XR_XIR_I64,0,3},cell={0},read={0};
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)258,&scalar,&admission,&cell)==XR_XIR_VALUE_OK);
    const XrXirCellAuthority parent={domain,arena,11},wrong={domain,&cell,12},invalid={domain,arena,0};
    XrXirCellLoan exclusive={0};XrXirCellReadLoan loan={0};
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&exclusive)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&cell,&exclusive,&parent);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&wrong,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&invalid,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&parent,&admission,&loan)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_commit(&cell,&loan,&parent);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(xr_xir_cell_authorized_read(&cell,&wrong,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(xr_xir_cell_authorized_read(&cell,&parent,&read)==XR_XIR_VALUE_OK && read.payload==3);
    xr_xir_value_drop(&read);scalar.payload=4;
    CHECK(xr_xir_cell_authorized_write(&cell,&parent,&scalar,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_read_loan_release(&loan);
    CHECK(xr_xir_cell_authorized_write(&cell,&parent,&scalar,&admission)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&exclusive);
    CHECK(xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK && read.payload==4);xr_xir_value_drop(&read);
    xr_xir_value_drop(&cell);xr_xir_domain_drop(domain);CHECK(!live);
}
static void read_loan_rejections(XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(read_loan_domain(&domain)==XR_XIR_VALUE_OK && read_loan_domain(&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,0};
    XrXirValue scalar={XR_XIR_I64,0,5},cell={0};XrXirCellReadLoan loan={0};
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)258,&scalar,&admission,&cell)==XR_XIR_VALUE_OK);
    const XrXirCellAuthority authority={domain,arena,13};
    CHECK(xr_xir_cell_read_loan_prepare(NULL,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&scalar,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,NULL,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,NULL,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,NULL)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue damaged=cell;damaged.reserved=1;
    CHECK(xr_xir_cell_read_loan_prepare(&damaged,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    XirCell *body=(XirCell *)object_pointer(&cell);size_t bytes=body->allocation_bytes;
    body->allocation_bytes=bytes-1;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    body->allocation_bytes=bytes;body->value.reserved=1;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    body->value.reserved=0;CHECK(xr_xir_value_valid(&cell));
    admission.domain=foreign;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loan)==XR_XIR_VALUE_BAD_ARGUMENT && !loan.cell);
    admission.domain=domain;
    uint64_t work=xr_xir_domain_budget_stats(domain).work,limit=domain->budget.work_limit;
    domain->budget.work_limit=work+XR_XIR_CELL_READ_LOAN_WORK-1;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loan)==XR_XIR_VALUE_LIMIT && !loan.cell);
    CHECK(xr_xir_cell_unborrowed(&cell));
    domain->budget.work_limit=work+XR_XIR_CELL_READ_LOAN_WORK;
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&loan)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_abort(&loan);
    CHECK(xr_xir_domain_budget_stats(domain).work==work+XR_XIR_CELL_READ_LOAN_WORK);
    domain->budget.work_limit=limit;
    CHECK(xr_xir_cell_unborrowed(&cell));xr_xir_value_drop(&cell);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);CHECK(!live);
}
static void read_loan_late_owner(XrXirTypeArena **arena) {
    XrXirDomain *domain=NULL;CHECK(read_loan_domain(&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={*arena,domain,NULL,NULL,1000000,65536};
    XrXirValue scalar={XR_XIR_I64,0,31},cell={0},alias={0},read={0};
    CHECK(xr_xir_cell_new(domain,*arena,(XrXirType)258,&scalar,&admission,&cell)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&cell,&alias)==XR_XIR_VALUE_OK);
    const XrXirCellAuthority authority={domain,*arena,19};XrXirCellReadLoan first={0},second={0};
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,&admission,&first)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_commit(&cell,&first,&authority);
    CHECK(xr_xir_cell_read_loan_prepare(&alias,&authority,&admission,&second)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_commit(&alias,&second,&authority);
    xr_xir_value_drop(&cell);xr_xir_compile_type_arena_drop(*arena);*arena=NULL;
    size_t before=calls;xr_xir_domain_drop(domain);
    CHECK(xr_xir_cell_read(&alias,&read)==XR_XIR_VALUE_OK && read.payload==31);xr_xir_value_drop(&read);
    xr_xir_cell_read_loan_release(&first);
    CHECK(!xr_xir_cell_unborrowed(&alias));xr_xir_cell_read_loan_release(&second);
    CHECK(xr_xir_cell_unborrowed(&alias));xr_xir_value_drop(&alias);
    CHECK(calls==before && !live && !value_compile_live && !value_compile_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--full")));
    bool full=argc==2;XrXirTypeArena *arena=read_loan_arena();size_t first=calls;
    CHECK(read_loan_sequence(arena)==XR_XIR_VALUE_OK);size_t sites=calls-first;
    CHECK(sites>3);
    if (full) for (size_t point=0;point<sites;++point) {
        fail_at=calls+point;
        XrXirValueStatus status=read_loan_sequence(arena);fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_VALUE_OOM && !live);
    }
    read_loan_authority(arena);read_loan_rejections(arena);read_loan_late_owner(&arena);
    CHECK(!arena && !live && !value_compile_live && !value_compile_bytes);
    printf("Shared Cell root loans: every mutable route denied, non-LIFO release, exact work, authority, COW and late owner PASS; allocation sites=%zu mode=%s\n",sites,full?"full":"baseline");
    return 0;
}
