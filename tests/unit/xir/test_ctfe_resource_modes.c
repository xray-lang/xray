/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ctfe_resource_modes.c - Genuine compiler memory and execution policies
 */
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)

static void compiler_values(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrCompileResources *memory=NULL;
    CHECK(xr_compile_resources_new(&limits,&memory)==XR_COMPILE_RESOURCE_OK);
    XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_domain_new(memory,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_is_compile(domain));
    XrXirDomainBudgetControls controls={UINT64_C(1048576),UINT64_C(1048576),10000,65536,65536};
    CHECK(xr_xir_domain_budget_bind(domain,&controls)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!xr_xir_domain_work(domain,0) && !xr_xir_domain_work(domain,1));
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    CHECK(!xr_xir_domain_metadata_allocate(domain,64,&status) && status==XR_XIR_VALUE_BAD_ARGUMENT);
    status=XR_XIR_VALUE_OK;
    CHECK(!xr_xir_domain_call_allocate(domain,64,&status) && status==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue text={0},needle={0},replacement={0},result={0};
    CHECK(xr_xir_string_new(domain,"ababa",5,&text)==XR_XIR_VALUE_OK);
    XrXirDomain *borrowed=NULL;
    CHECK(xr_xir_compile_value_domain_borrow(memory,&text,NULL,&borrowed)==XR_XIR_VALUE_OK && borrowed==domain);
    CHECK(xr_xir_string_new(domain,"ba",2,&needle)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"X",1,&replacement)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.domain=domain,.scratch_bytes=65536};
    CHECK(xr_xir_value_admit(&text,XR_XIR_STRING,&admission)==XR_XIR_VALUE_OK && admission.work==0);
    XrXirValue invalid=text;invalid.reserved=1;
    CHECK(xr_xir_value_admit(&invalid,XR_XIR_STRING,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_string_replace(&text,&needle,&replacement,true,&admission,&result)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&result,&bytes,&length) && length==3 && !memcmp(bytes,"aXX",3));
    CHECK(admission.work==0 && !xr_xir_domain_budget_stats(domain).bound);
    xr_xir_value_drop(&result);xr_xir_value_drop(&replacement);xr_xir_value_drop(&needle);xr_xir_value_drop(&text);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(memory,&before)==XR_COMPILE_RESOURCE_OK);
    xr_xir_domain_drop(domain);
    CHECK(xr_compile_resources_stats(memory,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes<before.held_bytes);
    xr_compile_resources_release(memory);
}
static void execution_work(void) {
    XrXirDomainBudgetControls controls={UINT64_C(1048576),UINT64_C(1048576),10000,65536,65536};
    XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new_budgeted(UINT64_C(1048576),&controls,&domain)==XR_XIR_VALUE_OK);
    CHECK(!xr_xir_domain_is_compile(domain));
    XrXirDomainBudgetStats first=xr_xir_domain_budget_stats(domain);
    CHECK(xr_xir_domain_work(domain,3));
    XrXirDomainBudgetStats paid=xr_xir_domain_budget_stats(domain);
    CHECK(paid.work==first.work+3 && paid.bound);
    CHECK(!xr_xir_domain_work(domain,paid.work_limit));
    CHECK(xr_xir_domain_budget_stats(domain).work==paid.work);
    XrXirValue text={0};CHECK(xr_xir_string_new(domain,"runtime",7,&text)==XR_XIR_VALUE_OK);
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *memory=NULL;
    CHECK(xr_compile_resources_new(&limits,&memory)==XR_COMPILE_RESOURCE_OK);
    XrXirDomain *borrowed=NULL;
    CHECK(xr_xir_compile_value_domain_borrow(memory,&text,NULL,&borrowed)==XR_XIR_VALUE_BAD_ARGUMENT && !borrowed);
    xr_compile_resources_release(memory);
    XrXirValueAdmission admission={.domain=domain,.scratch_bytes=65536};
    CHECK(xr_xir_value_admit(&text,XR_XIR_STRING,&admission)==XR_XIR_VALUE_LIMIT);
    admission.work=2;uint64_t before=xr_xir_domain_budget_stats(domain).work;
    CHECK(xr_xir_value_admit(&text,XR_XIR_STRING,&admission)==XR_XIR_VALUE_OK && admission.work==1);
    CHECK(xr_xir_domain_budget_stats(domain).work==before+1);
    xr_xir_value_drop(&text);xr_xir_domain_drop(domain);
}
static void finite_memory(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,4096,false};
    XrCompileResources *memory=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_compile_resources_new(&limits,&memory)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_domain_new(memory,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    char bytes[8192];memset(bytes,'x',sizeof(bytes));XrXirValue output={0};
    CHECK(xr_xir_string_new(domain,bytes,sizeof(bytes),&output)==XR_XIR_VALUE_MEMORY_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload);
    void *allocation=NULL;
    CHECK(xr_compile_resources_calloc(memory,SIZE_MAX,2,&allocation)==XR_COMPILE_RESOURCE_OVERFLOW && !allocation);
    xr_xir_domain_drop(domain);xr_compile_resources_release(memory);
}
static bool cancel_now(const void *context) { return *(const bool *)context; }
static void compiler_type_families(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirCallableParameter fields[]={{(XrXirType)257,0},{XR_XIR_I64,0}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2}};
    XrXirTypes types={nodes,3,NULL,NULL};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_ctfe_type_admit(&context,&types,(XrXirType)258)==XR_XIR_CTFE_OK);
    XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=arena,.domain=domain,.scratch_bytes=65536};
    XrXirValue text={0},some={0},array={0},tuple={0},observed={0},reserved={0};
    CHECK(xr_xir_string_new(domain,"kept",4,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)256,&text,&admission,&some)==XR_XIR_VALUE_OK);
    XrXirValue elements[]={some,some};
    CHECK(xr_xir_array_new((XrXirType)257,elements,2,&admission,&array)==XR_XIR_VALUE_OK);
    XrXirValue members[]={array,{XR_XIR_I64,0,42}};
    CHECK(xr_xir_tuple_new((XrXirType)258,members,2,&admission,&tuple)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&tuple,(XrXirType)258,&admission)==XR_XIR_VALUE_OK && admission.work==0);
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,1,&admission,&observed,&fault)==XR_XIR_VALUE_OK);
    bool present=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(&observed,&present,&payload) && present && payload);
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(payload,&bytes,&length) && length==4 && !memcmp(bytes,"kept",4));
    CHECK(xr_xir_array_reserve(&array,8,&admission,&reserved)==XR_XIR_VALUE_OK && admission.work==0);
    XrXirValue rejected_output={0};
    CHECK(xr_xir_array_get(&array,2,&admission,&rejected_output,&fault)==XR_XIR_VALUE_BOUNDS && !rejected_output.type);
    xr_xir_value_drop(&reserved);xr_xir_value_drop(&observed);xr_xir_value_drop(&tuple);
    xr_xir_value_drop(&array);xr_xir_value_drop(&some);xr_xir_value_drop(&text);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    XrXirTypeNode rejected={.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256};
    XrXirTypes cycle={&rejected,1,NULL,NULL};
    CHECK(xr_xir_compile_ctfe_type_admit(&context,&cycle,(XrXirType)256)==XR_XIR_CTFE_NOT_SUPPORTED);
    rejected.element=XR_XIR_UNIT;
    CHECK(xr_xir_compile_ctfe_type_admit(&context,&cycle,(XrXirType)256)==XR_XIR_CTFE_NOT_SUPPORTED);
    rejected.kind=XR_XIR_TYPE_CELL;rejected.element=XR_XIR_I64;
    CHECK(xr_xir_compile_ctfe_type_admit(&context,&cycle,(XrXirType)256)==XR_XIR_CTFE_NOT_SUPPORTED);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);
}
static void borrowed_control(void) {
    bool cancelled=true;XrCompileRequestOptions options={.cancel_poll=cancel_now,.cancel_context=&cancelled};
    XrCompileRequestControl control={0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    /* Entry cancellation precedes artifact inspection and preserves output. */
    XrXirArtifact artifact={0};XrXirValue output={0};
    XrXirCtfeRequest request={.artifact=&artifact,.control=&control};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_CANCELLED);
    CHECK(!output.type && !output.reserved && !output.payload);
    XrCompileRequestOptions expired={.deadline_mode=XR_COMPILE_DEADLINE_AFTER,.timeout_ns=0};
    XrCompileRequestControl deadline={0};
    CHECK(xr_compile_request_control_init(&expired,&deadline)==XR_COMPILE_RESOURCE_OK);
    request.control=&deadline;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_DEADLINE);
}
int main(void) {
    compiler_values();execution_work();finite_memory();compiler_type_families();borrowed_control();
    puts("Compiler owner/value admission, execution work, exact memory failures and borrowed control PASS");
    return 0;
}
