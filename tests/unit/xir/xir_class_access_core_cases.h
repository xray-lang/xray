/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_access_core_cases.h - Whole and projected loans share one real Class
 */
#ifndef XIR_CLASS_ACCESS_CORE_CASES_H
#define XIR_CLASS_ACCESS_CORE_CASES_H
static void class_access_core_read(const XrXirValue *object,const XrXirCellAuthority *authority,
    XrXirValueAdmission *admission,int64_t expected) {
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)PREFIX_CLASS_T,0};
    XrXirValuePath path={&step,1};XrXirValue output={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_class_authorized_path_read(object,authority,&path,admission,&output,&fault)==XR_XIR_VALUE_OK);
    CHECK(output.type==XR_XIR_I64 && !output.reserved && output.payload==expected && xr_xir_fault_empty(fault));
    xr_xir_value_drop(&output);
}
static void class_access_core_loans(XrXirTypeArena *arena) {
    size_t blocks=cell_owner_live,bytes=cell_owner_bytes;
    XrXirDomain *domain=observation_core_domain(65536);XrXirValue object={0};
    publication_core_simple(arena,domain,&object);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,65536,65536};
    unsigned activation=0,frame=0,other_frame=0;
    /* These finite primitives test the storage transaction, not a Program grant. */
    XrXirCellAuthority parent={&activation,&frame,1},child={&activation,&other_frame,2};
    XrXirCellLoan whole={0},leaf={0};XrXirClassPublication publication={0};
    XirClassObject *identity=(XirClassObject *)object_pointer(&object);
    CHECK(xr_xir_class_access_loan_prepare(&object,&parent,&admission,&whole)==XR_XIR_VALUE_OK);
    CHECK(!whole.scope && whole.cell==identity && !whole.previous && !identity->borrow_top &&
        class_origin_state(identity)==XIR_CLASS_ORIGIN_PREPARED);
    CHECK(xr_xir_class_publication_prepare(&object,1,&admission,&publication)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    xr_xir_cell_loan_abort(&whole);CHECK(cell_loan_empty(&whole) && !class_origin_state(identity));
    CHECK(xr_xir_class_access_loan_prepare(&object,&parent,&admission,&whole)==XR_XIR_VALUE_OK);
    xr_xir_class_loan_commit(&object,&whole,&parent);
    CHECK(identity->borrow_top==&whole && !whole.scope &&
        class_origin_state(identity)==XIR_CLASS_ORIGIN_BORROWED);
    class_access_core_read(&object,&parent,&admission,40);
    XrXirCellLoan occupied=whole,occupied_before=occupied;
    uint64_t work=admission.work,domain_work=xr_xir_domain_budget_stats(domain).work;
    CHECK(xr_xir_class_access_loan_prepare(&object,&parent,&admission,&occupied)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied,&occupied_before,sizeof(occupied)) && admission.work==work &&
        xr_xir_domain_budget_stats(domain).work==domain_work);
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)PREFIX_CLASS_T,0};
    XrXirValuePath path={&step,1},empty={0};XrXirValue projection={0},output={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_cell_project_authorized((XrXirType)PREFIX_CELL_T,&object,&path,&parent,
        &admission,&projection)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_access_loan_prepare(&projection,&child,&admission,&leaf)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(cell_loan_empty(&leaf) && identity->borrow_top==&whole);
    CHECK(xr_xir_class_access_loan_prepare(&projection,&parent,&admission,&leaf)==XR_XIR_VALUE_OK);
    CHECK(leaf.previous==&whole && leaf.scope==cell_owner(&projection));
    xr_xir_cell_loan_commit(&projection,&leaf,&child);
    CHECK(identity->borrow_top==&leaf && identity->borrow_top->scope==cell_owner(&projection));
    CHECK(xr_xir_cell_authorized_path_read(&projection,&parent,&empty,&admission,&output,&fault)==
        XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&output));
    CHECK(xr_xir_class_authorized_path_read(&object,&child,&path,&admission,&output,&fault)==
        XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&output));
    CHECK(xr_xir_cell_authorized_path_read(&projection,&child,&empty,&admission,&output,&fault)==XR_XIR_VALUE_OK);
    CHECK(output.type==XR_XIR_I64 && output.payload==40);xr_xir_value_drop(&output);
    XrXirValue replacement={XR_XIR_I64,0,42};
    CHECK(xr_xir_cell_authorized_path_write(&projection,&child,&empty,&replacement,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_publication_prepare(&object,1,&admission,&publication)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    xr_xir_cell_loan_release(&leaf);CHECK(cell_loan_empty(&leaf) && identity->borrow_top==&whole);
    class_access_core_read(&object,&parent,&admission,42);
    xr_xir_cell_loan_release(&whole);CHECK(cell_loan_empty(&whole) && !identity->borrow_top && !class_origin_state(identity));
    CHECK(xr_xir_class_publication_prepare(&object,1,&admission,&publication)==XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication);xr_xir_class_publication_drop(&publication);
    CHECK(xr_xir_class_module_storage(&object) && xr_xir_cell_module_storage(&projection));
    CHECK(xr_xir_class_access_loan_prepare(&object,&parent,&admission,&whole)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_module_storage(&object));xr_xir_cell_loan_abort(&whole);
    CHECK(cell_loan_empty(&whole) && class_origin_state(identity)==XIR_CLASS_ORIGIN_MODULE);
    xr_xir_value_drop(&projection);xr_xir_value_drop(&object);
    xr_xir_domain_close(domain);xr_xir_domain_drop(domain);
    CHECK(cell_owner_live==blocks && cell_owner_bytes==bytes);
}
static size_t class_access_core_fault(XrXirTypeArena *arena,size_t ordinal,bool injected) {
    size_t blocks=cell_owner_live,bytes=cell_owner_bytes;
    XrXirDomain *domain=observation_core_domain(65536);XrXirValue object={0};
    publication_core_simple(arena,domain,&object);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,65536,65536};
    unsigned activation=0,frame=0;XrXirCellAuthority authority={&activation,&frame,1};
    XrXirCellLoan loan={0};XrXirValue output={0};XrXirFaultDetail fault={0};
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)PREFIX_CLASS_T,0};XrXirValuePath path={&step,1};
    observation_runtime_fault_begin(ordinal);observation_runtime_phase(OBSERVATION_FAULT_CLASS);
    CHECK(xr_xir_class_access_loan_prepare(&object,&authority,&admission,&loan)==XR_XIR_VALUE_OK);
    xr_xir_class_loan_commit(&object,&loan,&authority);
    XrXirValueStatus status=xr_xir_class_authorized_path_read(&object,&authority,&path,&admission,&output,&fault);
    size_t sites=observation_runtime_fault.attempts;
    if (injected) CHECK(status==XR_XIR_VALUE_OOM && observation_runtime_fault.hits==1 && unit_value(&output));
    else CHECK(status==XR_XIR_VALUE_OK && !observation_runtime_fault.hits && output.payload==40);
    size_t attempts=observation_runtime_fault.attempts;
    xr_xir_cell_loan_release(&loan);CHECK(observation_runtime_fault.attempts==attempts && cell_loan_empty(&loan));
    observation_runtime_fault_end();xr_xir_value_drop(&output);
    CHECK(!((XirClassObject *)object_pointer(&object))->borrow_top && !xr_xir_class_module_storage(&object));
    CHECK(xr_xir_class_access_loan_prepare(&object,&authority,&admission,&loan)==XR_XIR_VALUE_OK);
    xr_xir_class_loan_commit(&object,&loan,&authority);class_access_core_read(&object,&authority,&admission,40);
    xr_xir_cell_loan_release(&loan);
    XrXirDomainBudgetStats stats=xr_xir_domain_budget_stats(domain);
    printf("Class access primitive ordinal=%zu sites=%zu work=%llu requested=%llu\n",ordinal,sites,
        (unsigned long long)stats.work,(unsigned long long)stats.requested_bytes);
    xr_xir_value_drop(&object);xr_xir_domain_close(domain);xr_xir_domain_drop(domain);
    CHECK(cell_owner_live==blocks && cell_owner_bytes==bytes);return sites;
}
static size_t class_access_core_module_cycle(ValueCompileOwner *compiler,size_t ordinal,bool injected) {
    size_t blocks=cell_owner_live,bytes=cell_owner_bytes;
    XrXirTypeArena *arena=observation_core_cycle_arena(compiler);
    XrXirDomain *domain=observation_core_domain(65536),*foreign=observation_core_domain(65536);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,65536,65536};
    XrXirValue none={0},root={0},child={0},some_root={0},some_child={0};
    CHECK(xr_xir_nullable_new((XrXirType)257,NULL,&admission,&none)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256,&none,1,&admission,&root)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256,&none,1,&admission,&child)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&root,&admission,&some_root)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&child,&admission,&some_child)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&child,0,&some_root,&admission)==XR_XIR_VALUE_OK);
    XrXirClassPublication publication={0};
    CHECK(xr_xir_class_publication_prepare(&root,1,&admission,&publication)==XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication);xr_xir_class_publication_drop(&publication);
    unsigned activation=0,frame=0;XrXirCellAuthority authority={&activation,&frame,1};
    XrXirCellLoan whole={0};XirClassObject *identity=(XirClassObject *)object_pointer(&root);
    CHECK(xr_xir_class_access_loan_prepare(&root,&authority,&admission,&whole)==XR_XIR_VALUE_OK);
    xr_xir_class_loan_commit(&root,&whole,&authority);
    CHECK(xr_xir_class_publication_prepare(&some_root,1,&admission,&publication)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && identity->borrow_top==&whole);
    XrXirValueAdmission wrong=admission;wrong.domain=foreign;
    uint64_t before=wrong.work,foreign_work=xr_xir_domain_budget_stats(foreign).work;
    CHECK(class_publication_prepare_route(&some_root,1,&wrong,&identity->object,&publication)==
        XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && wrong.work==before &&
        xr_xir_domain_budget_stats(foreign).work==foreign_work && !xr_xir_class_module_storage(&child));
    XrXirCellLoan different=whole;
    XrXirClassPublication changed={.borrow_owner=identity,.borrow_loan=&different};
    CHECK(!class_publication_borrowed(&changed,identity));
    XrXirValuePathStep step={XR_XIR_PATH_FIELD,(XrXirType)256,0};XrXirValuePath path={&step,1};
    XrXirFaultDetail fault={0};
    observation_runtime_fault_begin(ordinal);observation_runtime_phase(OBSERVATION_FAULT_CLASS);
    XrXirValueStatus status=xr_xir_class_authorized_path_write(&root,&authority,&path,
        &some_child,&admission,&fault);
    size_t sites=observation_runtime_fault.attempts;
    if (injected) {
        CHECK(status==XR_XIR_VALUE_OOM && observation_runtime_fault.hits==1 && !xr_xir_class_module_storage(&child));
        CHECK(identity->borrow_top==&whole && class_origin_state(identity)==
            (XIR_CLASS_ORIGIN_MODULE|XIR_CLASS_ORIGIN_BORROWED));
    } else CHECK(status==XR_XIR_VALUE_OK && !observation_runtime_fault.hits && xr_xir_fault_empty(fault));
    observation_runtime_fault_end();
    if (injected) {
        XrXirValue previous={0};bool present=true;const XrXirValue *payload=NULL;
        CHECK(xr_xir_class_authorized_path_read(&root,&authority,&path,&admission,&previous,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&previous,&present,&payload) && !present && !payload);
        xr_xir_value_drop(&previous);
        CHECK(xr_xir_class_authorized_path_write(&root,&authority,&path,&some_child,&admission,&fault)==XR_XIR_VALUE_OK);
    }
    CHECK(identity->borrow_top==&whole && class_origin_state(identity)==
        (XIR_CLASS_ORIGIN_MODULE|XIR_CLASS_ORIGIN_BORROWED) && xr_xir_class_module_storage(&child));
    /* Walk the new child and its back edge, then break the cycle while the exact
     * same loan is still held. No public publication preparation accepts it. */
    CHECK(xr_xir_class_authorized_path_write(&root,&authority,&path,&none,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(identity->borrow_top==&whole && xr_xir_class_module_storage(&root));
    xr_xir_cell_loan_release(&whole);CHECK(!identity->borrow_top && class_origin_state(identity)==XIR_CLASS_ORIGIN_MODULE);
    xr_xir_value_drop(&some_child);xr_xir_value_drop(&some_root);xr_xir_value_drop(&child);
    xr_xir_value_drop(&root);xr_xir_value_drop(&none);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_close(domain);xr_xir_domain_drop(domain);
    xr_xir_domain_close(foreign);xr_xir_domain_drop(foreign);
    CHECK(cell_owner_live==blocks && cell_owner_bytes==bytes);
    printf("Class MODULE cycle ordinal=%zu sites=%zu rollback=%u\n",ordinal,sites,(unsigned)injected);
    return sites;
}
static void class_access_core_cases(ValueCompileOwner *compiler,XrXirTypeArena *arena) {
    class_access_core_loans(arena);
    size_t cycle_sites=class_access_core_module_cycle(compiler,SIZE_MAX,false);
    for (size_t ordinal=0;ordinal<cycle_sites;++ordinal) class_access_core_module_cycle(compiler,ordinal,true);
    size_t sites=class_access_core_fault(arena,SIZE_MAX,false);
    for (size_t ordinal=0;ordinal<sites;++ordinal) class_access_core_fault(arena,ordinal,true);
}
#endif // XIR_CLASS_ACCESS_CORE_CASES_H
