/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_class.h - Actual Class identities with scoped receiving views
 */
#ifndef TYPE_RECEIVING_CLASS_H
#define TYPE_RECEIVING_CLASS_H
#include "xir/xxir_class.h"
#include "xir/xxir_class_origin_internal.h"
#include "xir/xxir_cell_owner_internal.h"
static void received_class_storage(SourceFixtureOwner *compiler) {
    const XrXirNominalFieldIdentity field={{"value",5},XR_XIR_FIELD_MUTABLE};
    const XrXirNominalIdentity identity={{"owner",5},{"Item",4},1,0,&field,1,
        XR_XIR_NOMINAL_CLASS,NULL,0,0,{0}};
    const XrXirNominalTable table={NULL,1,&identity};const XrXirType field_type=XR_XIR_I64;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&field_type,1}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&field_type,1}},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}};
    XrXirTypes pools[]={{&node,1,&table,NULL},{shifted,3,&table,NULL}};
    XrXirTypeArena *source=NULL,*target=NULL,*unrelated=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&source)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&unrelated)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(0),type(1)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1024*1024,&foreign)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue scalar={XR_XIR_I64,0,41},object={0},read={0},projection={0};
    CHECK(xr_xir_class_new(type(0),&scalar,1,&admission,&object)==XR_XIR_VALUE_OK);
    XrXirClassPublication publication={0};
    CHECK(xr_xir_class_publication_prepare(&object,1,&admission,&publication)==XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication);xr_xir_class_publication_drop(&publication);
    admission.arena=target;scalar.payload=51;
    CHECK(xr_xir_class_publication_prepare(&object,1,&admission,&publication)==XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication);xr_xir_class_publication_drop(&publication);
    CHECK(xr_xir_class_set(&object,0,&scalar,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_get(&object,0,&admission,&read)==XR_XIR_VALUE_OK && read.payload==51);
    xr_xir_value_drop(&read);
    CHECK(object.type==(uint32_t)type(0) && xr_xir_value_arena(&object)==source);
    XrXirCellAuthority authority={compiler,&object,1},child={compiler,&projection,2},wrong={compiler,&domain,3};
    XrXirCellLoan whole={0},leaf={0};XrXirFaultDetail fault={0};
    uint64_t work=admission.work;admission.work=0;
    CHECK(xr_xir_class_loan_prepare(&object,&authority,&admission,&whole)==XR_XIR_VALUE_LIMIT && !whole.cell);
    admission.work=work;
    CHECK(xr_xir_class_loan_prepare(&object,&authority,&admission,&whole)==XR_XIR_VALUE_OK);
    xr_xir_class_loan_commit(&object,&whole,&authority);
    const XrXirValuePathStep step={XR_XIR_PATH_FIELD,type(1),0};const XrXirValuePath path={&step,1},empty={0};
    CHECK(xr_xir_class_authorized_path_read(&object,&authority,&path,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==51);
    xr_xir_value_drop(&read);
    scalar.payload=61;
    CHECK(xr_xir_class_authorized_path_write(&object,&authority,&path,&scalar,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_project_authorized(type(2),&object,&path,&authority,&admission,&projection)==XR_XIR_VALUE_OK);
    CHECK(projection.type==(uint32_t)type(2) && xr_xir_value_arena(&projection)==target);
    CHECK(xr_xir_value_admit(&projection,type(2),&admission)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&whole);
    CHECK(xr_xir_cell_loan_prepare_path(&projection,&authority,&admission,&fault,&leaf)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection,&leaf,&child);
    scalar.payload=73;
    CHECK(xr_xir_cell_authorized_path_write(&projection,&wrong,&empty,&scalar,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_path_write(&projection,&child,&empty,&scalar,&admission,&fault)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&leaf);xr_xir_value_drop(&projection);
    CHECK(xr_xir_class_get(&object,0,&admission,&read)==XR_XIR_VALUE_OK && read.payload==73);
    xr_xir_value_drop(&read);admission.arena=unrelated;
    CHECK(xr_xir_class_get(&object,0,&admission,&read)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    admission.arena=target;admission.domain=foreign;
    CHECK(xr_xir_class_get(&object,0,&admission,&read)==XR_XIR_VALUE_OK && read.payload==73);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_value_admit(&object,type(1),&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_set(&object,0,&scalar,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_class_loan_prepare(&object,&authority,&admission,&whole)==XR_XIR_VALUE_BAD_ARGUMENT && !whole.cell);
    xr_xir_value_drop(&object);xr_xir_compile_type_arena_drop(unrelated);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_CLASS_H
