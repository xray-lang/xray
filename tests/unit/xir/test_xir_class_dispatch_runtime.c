/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_dispatch_runtime.c - Ancestor views retain complete class owners
 *
 * KEY CONCEPT:
 *   Static field bounds never change the actual object or its release obligations.
 */
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include "xir/xxir_class_view_internal.h"
#include "xir/xxir_instance_function_internal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static size_t runtime_calls,runtime_live,runtime_fail=SIZE_MAX;
static void *class_test_malloc(size_t bytes) {
    if (runtime_calls++==runtime_fail) return NULL;
    void *p=xr_malloc(bytes);if (p) ++runtime_live;return p;
}
static void *class_test_calloc(size_t count,size_t bytes) {
    if (runtime_calls++==runtime_fail) return NULL;
    void *p=xr_calloc(count,bytes);if (p) ++runtime_live;return p;
}
static void *class_test_realloc(void *p,size_t bytes) {
    if (runtime_calls++==runtime_fail) return NULL;
    bool was_null=!p;void *next=xr_realloc(p,bytes);if (next && was_null) ++runtime_live;return next;
}
static void class_test_free(void *p) {
    if (p) { CHECK(runtime_live);--runtime_live; }xr_free(p);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) class_test_malloc(bytes)
#define xr_calloc(count,bytes) class_test_calloc(count,bytes)
#define xr_realloc(p,bytes) class_test_realloc(p,bytes)
#define xr_free(p) class_test_free(p)
#include "xir/xxir_value.c"
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
enum { BASE=XR_XIR_CONSTRUCTED_TYPE_BASE,DOG,OTHER,BASE_ARRAY,DOG_ARRAY };
static XrXirTypeArena *class_test_arena(XrXirCompileContext *compiler) {
    XrXirNominalFieldIdentity fields[]={{{"name",4},0},{{"breed",5},0}};
    XrXirNominalIdentity identities[]={
        {.module={"test",4},.name={"Base",4},.exported=1,.fields=fields,
            .field_count=1,.kind=XR_XIR_NOMINAL_CLASS},
        {.module={"test",4},.name={"Dog",3},.exported=1,.fields=fields,
            .field_count=2,.kind=XR_XIR_NOMINAL_CLASS,.parent=1},
        {.module={"test",4},.name={"Other",5},.exported=1,.fields=fields,
            .field_count=1,.kind=XR_XIR_NOMINAL_CLASS}};
    XrXirNominalTable table={.count=3,.identities=identities};
    XrXirType field_types[]={XR_XIR_STRING,XR_XIR_STRING};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0,.fields=field_types,.field_count=1}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1,.fields=field_types,.field_count=2}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=2,.fields=field_types,.field_count=1}},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)BASE},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)DOG}};
    XrXirTypes types={nodes,5,&table,NULL};XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(compiler,&types,&arena)==XR_XIR_VALUE_OK);
    return arena;
}
static void class_test_views(XrXirTypeArena *arena,XrXirDomain *domain) {
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;
    XrXirValue fields[2]={{0}},dog={0},base={0},view={0},result={0},array={0},dogs={0};
    CHECK(xr_xir_string_new(domain,"Rex",3,&fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"Labrador",8,&fields[1])==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    CHECK(xr_xir_class_new((XrXirType)DOG,fields,2,&admission,&dog)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_from_payload(arena,(XrXirType)BASE,dog.payload,&view));
    CHECK(view.type==BASE && view.payload==dog.payload && object_pointer(&view)->type==(XrXirType)DOG);
    CHECK(xr_xir_value_argument(&dog,arena,(XrXirType)BASE));
    CHECK(!xr_xir_value_argument(&view,arena,(XrXirType)DOG));
    XrXirValue sentinel={XR_XIR_I64,0,99};
    CHECK(!xr_xir_value_from_payload(arena,(XrXirType)OTHER,dog.payload,&sentinel));
    CHECK(sentinel.type==XR_XIR_I64 && sentinel.payload==99);
    size_t before=runtime_calls;
    CHECK(xr_xir_value_copy(&view,&base)==XR_XIR_VALUE_OK && runtime_calls==before);
    CHECK(xr_xir_class_get(&base,1,&admission,&result)==XR_XIR_VALUE_BAD_ARGUMENT && unit_value(&result));
    CHECK(xr_xir_class_set(&base,1,&fields[0],&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_new((XrXirType)BASE_ARRAY,&dog,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)DOG_ARRAY,&dog,1,&admission,&dogs)==XR_XIR_VALUE_OK);
    CHECK(!xr_xir_value_argument(&dogs,arena,(XrXirType)BASE_ARRAY));
    XrXirValue rejected={0};
    CHECK(xr_xir_array_new((XrXirType)DOG_ARRAY,&base,1,&admission,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(unit_value(&rejected));
    XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,0,&admission,&result,&fault)==XR_XIR_VALUE_OK);
    CHECK(result.type==BASE && result.payload==dog.payload);
    XrXirType actual=XR_XIR_UNIT;
    CHECK(xir_class_actual_type(&result,arena,&actual)==XR_XIR_VALUE_OK && actual==(XrXirType)DOG);
    xr_xir_value_drop(&result);xr_xir_value_drop(&dogs);xr_xir_value_drop(&array);
    xr_xir_value_drop(&dog);xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);
    CHECK(xr_xir_class_get(&base,0,&admission,&result)==XR_XIR_VALUE_OK);
    const char *text=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&result,&text,&length) && length==3 && !memcmp(text,"Rex",3));
    CHECK(object_pointer(&base)->type==(XrXirType)DOG);
    before=runtime_calls;xr_xir_value_drop(&result);xr_xir_value_drop(&base);
    CHECK(runtime_calls==before && xr_xir_domain_stats(domain).live_bytes==baseline);
}
static void class_test_reservations(XrXirTypeArena *arena,XrXirTypeArena *foreign,XrXirDomain *domain) {
    XrXirValue fields[2]={{0}},dog={0};
    CHECK(xr_xir_string_new(domain,"Rex",3,&fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"Dog",3,&fields[1])==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
    CHECK(xr_xir_class_new((XrXirType)DOG,fields,2,&admission,&dog)==XR_XIR_VALUE_OK);
    XrXirClassPublication publication={0};XrXirType actual=XR_XIR_I64;
    CHECK(xr_xir_class_publication_prepare(&dog,1,&admission,&publication)==XR_XIR_VALUE_OK);
    CHECK(xir_class_actual_type(&dog,arena,&actual)==XR_XIR_VALUE_BAD_ARGUMENT && actual==XR_XIR_I64);
    xr_xir_class_publication_drop(&publication);
    CHECK(xir_class_actual_type(&dog,arena,&actual)==XR_XIR_VALUE_OK && actual==(XrXirType)DOG);
    unsigned activation=0,frame=0;XrXirCellAuthority authority={&activation,&frame,1};XrXirCellLoan loan={0};
    CHECK(xr_xir_class_loan_prepare(&dog,&authority,&admission,&loan)==XR_XIR_VALUE_OK);
    CHECK(xir_class_actual_type(&dog,arena,&actual)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_class_loan_commit(&dog,&loan,&authority);
    CHECK(xir_class_actual_type(&dog,arena,&actual)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_loan_release(&loan);
    CHECK(xir_class_actual_type(&dog,arena,&actual)==XR_XIR_VALUE_OK);
    actual=XR_XIR_I64;
    CHECK(xir_class_actual_type(&dog,foreign,&actual)==XR_XIR_VALUE_BAD_ARGUMENT && actual==XR_XIR_I64);
    xr_xir_value_drop(&dog);xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);
}
static void class_test_failures(XrXirTypeArena *arena,XrXirDomain *domain) {
    XrXirValue fields[2]={{0}};
    CHECK(xr_xir_string_new(domain,"Rex",3,&fields[0])==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"Dog",3,&fields[1])==XR_XIR_VALUE_OK);
    uint64_t baseline=xr_xir_domain_stats(domain).live_bytes;size_t live=runtime_live,sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        XrXirValue dog={0};XrXirValueAdmission admission={arena,domain,NULL,NULL,1000000,65536};
        size_t before=runtime_calls;runtime_fail=pass?before+pass-1:SIZE_MAX;
        XrXirValueStatus status=xr_xir_class_new((XrXirType)DOG,fields,2,&admission,&dog);
        if (!pass) { CHECK(status==XR_XIR_VALUE_OK);sites=runtime_calls-before;CHECK(sites); }
        else CHECK(status==XR_XIR_VALUE_OOM && unit_value(&dog) && runtime_calls>runtime_fail);
        runtime_fail=SIZE_MAX;before=runtime_calls;xr_xir_value_drop(&dog);
        CHECK(runtime_calls==before && runtime_live==live && xr_xir_domain_stats(domain).live_bytes==baseline);
    }
    printf("class allocation failures=%zu; every output empty and owner baseline restored\n",sites);
    xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);
}
int main(void) {
    XrCompileResourceLimits limits={0};XrXirCompileContext compiler={0};
    compiler.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&compiler.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeArena *arena=class_test_arena(&compiler),*foreign=class_test_arena(&compiler);XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(1048576,&domain)==XR_XIR_VALUE_OK);
    class_test_views(arena,domain);class_test_reservations(arena,foreign,domain);class_test_failures(arena,domain);
    XrXirValue output={0};
    CHECK(xr_xir_instance_class_method_at(NULL,0,NULL,&output)==XR_XIR_CALL_BAD_ARGUMENT && unit_value(&output));
    xr_xir_domain_drop(domain);CHECK(!runtime_live);xr_xir_compile_type_arena_drop(arena);
    xr_xir_compile_type_arena_drop(foreign);
    CHECK(xr_compile_resources_stats(compiler.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.held_bytes==after.held_bytes);xr_compile_resources_release(compiler.resources);
    puts("Class ancestor views: actual Derived fields retained and released; no whole Array covariance");
    return 0;
}
