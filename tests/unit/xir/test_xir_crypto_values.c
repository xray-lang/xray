/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_crypto_values.c - Actual byte storage, scoped projections and rollback
 *
 * KEY CONCEPT:
 *   Private loan primitives model authenticated driver frames separately from
 *   Source execution. The real provider runs only after transactional preparation.
 */
#include "xir/xxir_crypto_value.h"
#include "xir/xxir_array.h"
#include "base/xresource_limits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"crypto value %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xxir_type_arena.h"
static size_t crypto_allocations,crypto_fail_at=SIZE_MAX;
static bool fail_allocation(void){return crypto_allocations++==crypto_fail_at;}
#define CELL_OWNER_ALLOCATION_FAIL fail_allocation
#include "cell_owner_value_physical.h"
typedef struct CryptoGate {unsigned calls;bool allow;} CryptoGate;
static bool crypto_gate(void *data){CryptoGate *gate=data;++gate->calls;return gate->allow;}
static void check_bytes(const XrXirValue *value,unsigned byte,size_t length,size_t capacity) {
    const XirArray *array=(const XirArray *)object_pointer(value);
    CHECK(array->length==length && array->capacity==capacity && array->stride==1);
    for(size_t i=0;i<length;++i)CHECK(array->data[i]==byte);
}
static void fill_case(XrXirTypeArena *arena,bool projected,bool shared,bool empty,unsigned failure) {
    XrXirDomain *domain=NULL,*foreign=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(1048576,&foreign)==XR_XIR_VALUE_OK);
    XrXirDomainStats frame=xr_xir_domain_stats(domain);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1048576,1048576};
    XrXirValue fill={XR_XIR_U8,0,11},initial={0},array={0},outer={0},cell={0},projection={0},alias={0};
    CHECK(xr_xir_array_repeat((XrXirType)256,empty?0:4,&fill,&admission,&initial)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_reserve(&initial,8,&admission,&array)==XR_XIR_VALUE_OK);xr_xir_value_drop(&initial);
    if(shared)CHECK(xr_xir_value_copy(&array,&alias)==XR_XIR_VALUE_OK);
    if(projected)CHECK(xr_xir_array_new((XrXirType)257,&array,1,&admission,&outer)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)(projected?259:258),projected?&outer:&array,&admission,&cell)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);xr_xir_value_drop(&outer);
    unsigned activation=0,parent_frame=0,child_frame=0;
    XrXirCellAuthority parent={&activation,&parent_frame,1},child={&activation,&child_frame,2},wrong=parent;
    wrong.epoch=3;XrXirCellLoan parent_loan={0},child_loan={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL,&parent_loan)==XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&cell,&parent_loan,&parent);
    const XrXirValue *destination=&cell;const XrXirCellAuthority *authority=&parent;
    if(projected) {
        XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)257,0};XrXirValuePath path={&step,1};
        CHECK(xr_xir_cell_project_authorized((XrXirType)258,&cell,&path,&parent,&admission,&projection)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_cell_loan_prepare_path(&projection,&parent,&admission,&fault,&child_loan)==XR_XIR_VALUE_OK);
        xr_xir_cell_loan_commit(&projection,&child_loan,&child);destination=&projection;authority=&child;
    }
    CryptoGate gate={0,true};XirCryptoCommit commit={crypto_gate,&gate};
    CHECK(xir_cell_crypto_fill(destination,&wrong,&admission,&fault,&commit)==XR_XIR_VALUE_BAD_ARGUMENT && !gate.calls);
    CHECK(xir_cell_crypto_fill(destination,NULL,&admission,&fault,&commit)==XR_XIR_VALUE_BAD_ARGUMENT && !gate.calls);
    XrXirValueAdmission other=admission;other.domain=foreign;
    CHECK(xir_cell_crypto_fill(destination,authority,&other,&fault,&commit)==XR_XIR_VALUE_BAD_ARGUMENT && !gate.calls);
    XrXirValueAdmission limited=admission;limited.work=0;
    CHECK(xir_cell_crypto_fill(destination,authority,&limited,&fault,&commit)==XR_XIR_VALUE_LIMIT && !gate.calls);
    size_t live=cell_owner_live,bytes=cell_owner_bytes;
    if(failure==1)crypto_fail_at=crypto_allocations;
    if(failure==2)gate.allow=false;
    XrXirValueStatus status=xir_cell_crypto_fill(destination,authority,&admission,&fault,&commit);
    bool injected=failure==1 && crypto_allocations>crypto_fail_at;crypto_fail_at=SIZE_MAX;
    if(failure) {
        CHECK(status==(failure==1?XR_XIR_VALUE_OOM:XR_XIR_VALUE_BAD_ARGUMENT));
        CHECK(gate.calls==(failure==1?0u:1u) && (failure!=1 || injected));
        CHECK(cell_owner_live==live && cell_owner_bytes==bytes);
    } else CHECK(status==XR_XIR_VALUE_OK && gate.calls==1);
    XrXirValue actual={0},root={0};
    if(projected) {xr_xir_cell_loan_release(&child_loan);xr_xir_value_drop(&projection);}
    CHECK(xr_xir_cell_authorized_read(&cell,&parent,&root)==XR_XIR_VALUE_OK);
    if(projected)CHECK(xr_xir_array_get(&root,0,&admission,&actual,&fault)==XR_XIR_VALUE_OK);
    else {actual=root;root=(XrXirValue){0};}
    XirArray *result=(XirArray *)object_pointer(&actual);
    CHECK(result->length==(empty?0u:4u) && result->capacity==8 && result->stride==1);
    if(failure)check_bytes(&actual,11,empty?0:4,8);
    if(shared) {check_bytes(&alias,11,empty?0:4,8);if(!failure)CHECK(alias.payload!=actual.payload);}
    xr_xir_value_drop(&root);xr_xir_value_drop(&actual);xr_xir_cell_loan_release(&parent_loan);
    CHECK(xr_xir_cell_unborrowed(&cell));
    xr_xir_value_drop(&cell);xr_xir_value_drop(&alias);
    CHECK(xr_xir_domain_stats(domain).live_bytes==frame.live_bytes);
    xr_xir_domain_drop(foreign);xr_xir_domain_drop(domain);
    CHECK(!cell_owner_live && !cell_owner_bytes);
}
static void compare_cases(XrXirTypeArena *arena) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1048576,1048576};
    XrXirValue values[5]={{XR_XIR_U8,0,0},{XR_XIR_U8,0,11},{XR_XIR_U8,0,0},{XR_XIR_U8,0,255},{XR_XIR_U8,0,1}};
    XrXirValue original={0};CHECK(xr_xir_array_new((XrXirType)256,values,5,&admission,&original)==XR_XIR_VALUE_OK);
    for(unsigned axis=0;axis<6;++axis) {
        XrXirValue copy[5];memcpy(copy,values,sizeof(copy));size_t length=axis==4?0:axis==5?4:5;
        if(axis<3)copy[axis==0?0:axis==1?2:4].payload^=1;
        XrXirValue candidate={0};CHECK(xr_xir_array_new((XrXirType)256,copy,length,&admission,&candidate)==XR_XIR_VALUE_OK);
        size_t before=crypto_allocations;bool equal=true;
        CHECK(xir_crypto_timing_equal(&original,&candidate,&admission,&equal)==XR_XIR_VALUE_OK && equal==(axis==3));
        CHECK(crypto_allocations==before);xr_xir_value_drop(&candidate);
    }
    XrXirValue text={0};CHECK(xr_xir_string_new(domain,"u8",2,&text)==XR_XIR_VALUE_OK);bool unchanged=true;
    CHECK(xir_crypto_timing_equal(&original,&text,&admission,&unchanged)==XR_XIR_VALUE_BAD_ARGUMENT && unchanged);
    xr_xir_value_drop(&text);xr_xir_value_drop(&original);xr_xir_domain_drop(domain);
    CHECK(!cell_owner_live && !cell_owner_bytes);
}
int main(void) {
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},{.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)257}};
    XrXirTypes types={nodes,4,NULL,NULL};XrXirCompileContext context={0};
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    for(unsigned projected=0;projected<2;++projected)for(unsigned shared=0;shared<2;++shared)
        for(unsigned empty=0;empty<2;++empty)fill_case(arena,projected!=0,shared!=0,empty!=0,0);
    for(unsigned projected=0;projected<2;++projected)for(unsigned failure=1;failure<=2;++failure)
        fill_case(arena,projected!=0,true,false,failure);
    compare_cases(arena);xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
    puts("actual bytes, loan refusal, projected COW, OOM/gate rollback and no-heap comparison PASS");return 0;
}
