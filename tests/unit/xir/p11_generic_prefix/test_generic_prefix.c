/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_generic_prefix.c - First-failure ownership in generic metadata cloning
 *
 * KEY CONCEPT:
 *   One physical failure terminates sibling allocation; the original finite
 *   owner retains charges and supports a complete independent retry.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_interface.h"

static const XrCompileResourceLimits limits={UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static const XrXirType arguments[]={XR_XIR_I64,XR_XIR_STRING};
static const uint32_t kinds[]={XR_XIR_BINDER_TYPE,XR_XIR_BINDER_RESULT_VARIABLE};
static const XrXirInterfaceApplication application={0,arguments,2};
static const XrXirConstraint constraints[]={{0,&application,1},{0}};
static const XrXirGeneric generics[]={
    {constraints,2,arguments,2,kinds},{constraints,2,arguments,2,kinds}
};
static const XrXirModule module={.function_count=2,.generics=generics};
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats output={0};CHECK(xr_compile_resources_stats(context->resources,&output)==XR_COMPILE_RESOURCE_OK);return output;
}
static void check_copy(const XrXirGeneric *copy) {
    CHECK(copy && copy!=generics);
    for (uint32_t f=0;f<2;++f) {
        CHECK(copy[f].parameter_count==2 && copy[f].argument_count==2);
        CHECK(copy[f].constraints!=constraints && copy[f].constraints[0].interfaces!=&application);
        CHECK(copy[f].constraints[0].interfaces[0].arguments!=arguments);
        CHECK(copy[f].constraints[0].interfaces[0].arguments[1]==XR_XIR_STRING);
        CHECK(copy[f].arguments!=arguments && copy[f].arguments[1]==XR_XIR_STRING);
        CHECK(copy[f].parameter_kinds!=kinds && copy[f].parameter_kinds[1]==XR_XIR_BINDER_RESULT_VARIABLE);
    }
}
static size_t baseline(void) {
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before=stats(&context);XrXirGeneric *copy=NULL;
    size_t start=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_generics_clone(&context,&module,&copy, NULL)==XR_XIR_OK);check_copy(copy);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    xr_xir_compile_generics_free(copy,2);CHECK(stats(&context).live_bytes==before.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);return sites;
}
static void fault(size_t ordinal,size_t sites) {
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResources *same=context.resources;XrCompileResourceStats before=stats(&context);
    XrXirGeneric sentinel={0},*copy=&sentinel;size_t start=source_fixture_compile_attempts;
    source_fixture_compile_fail_at=start+ordinal;source_fixture_compile_injected=false;
    CHECK(xr_xir_compile_generics_clone(&context,&module,&copy, NULL)==XR_XIR_OUT_OF_MEMORY);
    CHECK(source_fixture_compile_injected && copy==&sentinel);
    CHECK(source_fixture_compile_attempts==start+ordinal+1);
    CHECK(stats(&context).live_bytes==before.live_bytes);
    XrCompileResourceStats failed=stats(&context);source_fixture_compile_fail_at=SIZE_MAX;
    start=source_fixture_compile_attempts;copy=NULL;
    CHECK(xr_xir_compile_generics_clone(&context,&module,&copy, NULL)==XR_XIR_OK);check_copy(copy);
    CHECK(source_fixture_compile_attempts==start+sites && context.resources==same);
    xr_xir_compile_generics_free(copy,2);XrCompileResourceStats retried=stats(&context);
    CHECK(retried.live_bytes==before.live_bytes && retried.work>=failed.work && retried.allocated_bytes>=failed.allocated_bytes);
    CHECK(retried.work<=limits.work && retried.allocated_bytes<=limits.allocated_bytes && retried.peak_bytes<=limits.live_bytes);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(void) {
    size_t sites=baseline();
    for (size_t ordinal=0;ordinal<sites;++ordinal) fault(ordinal,sites);
    printf("Generic clone full physical FI=%zu; first-error prefix, empty output, same-owner retry and physical0\n",sites);return 0;
}
