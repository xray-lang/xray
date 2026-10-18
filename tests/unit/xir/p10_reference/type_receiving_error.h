/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_receiving_error.h - Erased views keep actual enum owners across storage
 */
#ifndef TYPE_RECEIVING_ERROR_H
#define TYPE_RECEIVING_ERROR_H
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
static void received_error_arrays(SourceFixtureOwner *compiler) {
    const XrXirNominalVariant variants[]={{{"End",3},0,0},{{"Code",4},0,1}};
    const XrXirNominalFieldIdentity field={{"value",5},0};
    const XrXirNominalIdentity identity={{"owner",5},{"Status",6},1,0,&field,1,
        XR_XIR_NOMINAL_ENUM,variants,2,0,{0}};
    const XrXirNominalTable table={NULL,1,&identity};const XrXirType field_type=XR_XIR_I64;
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&field_type,1}},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_ERROR}};
    XrXirTypeNode shifted[]={{.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_BOOL},
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_ERROR}};
    XrXirTypes pools[]={{nodes,2,&table,NULL},{shifted,2,NULL,NULL}};
    XrXirTypeArena *source=NULL,*target=NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[0],&source)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_type_arena_new(&compiler->context,&pools[1],&target)==XR_XIR_VALUE_OK);
    XirTypeReceivingRoot root={type(1),type(1)};
    CHECK(xir_compile_type_arena_receive(&compiler->context,target,source,&root,1)==XR_XIR_VALUE_OK);
    CHECK(!xir_type_arena_same_type(source,type(0),target,type(0)));
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={source,domain,NULL,NULL,UINT64_C(4000000),128*1024};
    XrXirValue scalar={XR_XIR_I64,0,41},concrete={0},error={0},array={0},read={0};
    CHECK(xr_xir_enum_new(type(0),1,&scalar,1,&admission,&concrete)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_error_erase(&concrete,&admission,&error)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new(type(1),&error,1,&admission,&array)==XR_XIR_VALUE_OK);
    admission.arena=target;XrXirFaultDetail fault={0};XrXirValuePlace place={type(1),&array.payload};
    CHECK(xr_xir_array_get(&array,0,&admission,&read,&fault)==XR_XIR_VALUE_OK);
    CHECK(read.type==XR_XIR_ERROR && read.payload==error.payload && xr_xir_value_arena(&read)==source);
    xr_xir_value_drop(&read);
    const XrXirValuePathStep step={XR_XIR_PATH_INDEX,type(1),0};const XrXirValuePath path={&step,1};
    CHECK(xr_xir_value_path_write(&place,&path,&error,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&array,0,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.type==XR_XIR_ERROR);
    xr_xir_value_drop(&read);xr_xir_value_drop(&array);xr_xir_value_drop(&error);xr_xir_value_drop(&concrete);
    xr_xir_compile_type_arena_drop(target);xr_xir_compile_type_arena_drop(source);xr_xir_domain_drop(domain);
}
#endif // TYPE_RECEIVING_ERROR_H
