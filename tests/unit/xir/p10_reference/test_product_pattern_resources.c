/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_product_pattern_resources.c - Product Source stages and complete compiler allocation faults
 *
 * KEY CONCEPT:
 *   Every real compiler allocation may fail without publishing partial Source.
 *   Cumulative allocation, peak live and work bounds stay independent.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[20000];
static size_t attempts, fail_at = SIZE_MAX;
static uint64_t physical;
static bool baseline_only;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t i = 0; while (i < 20000 && allocations[i].pointer) ++i;
    CHECK(i < 20000); allocations[i] = (Allocation){memory, bytes}; physical += bytes;
    return memory;
}
static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0; while (i < 20000 && allocations[i].pointer != memory) ++i;
    CHECK(i < 20000); physical -= allocations[i].bytes; allocations[i] = (Allocation){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observed_alloc(n)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

typedef struct SourceCase { const char *source; XrXirStatus status; } SourceCase;
static const SourceCase cases[] = {
    {"export fn referenceAnswer()->i64{var ((a,b),{n:c})=((10,20),{n:12});a+=1;c-=1;return a+b+c}\n",XR_XIR_OK},
    {"var calls:i64=0\nfn make()->{z:i64,pair:(i64,i64)}{calls+=1;return {z:7,pair:(5,30)}}\n"
     "var {z:last,pair:(first,second)}=make()\n"
     "export fn referenceAnswer()->i64{first+=1;second-=1;return first+second+last+calls-1}\n",XR_XIR_OK},
    {"fn make<T:{n:i64,items:(i64,i64)}>(value:T)->fn()->i64{const {items:(a,b),n}=value;return fn()->i64{return a+b+n}}\n"
     "export fn referenceAnswer()->i64{const action=make({extra:true,items:(1,2),n:39});return action()}\n",XR_XIR_OK},
    {"fn bad()->i64{const ((n,),{v:n})=((1,),{v:2});return n}\n"
     "export fn referenceAnswer()->i64{return 42}\n",XR_XIR_BAD_STRUCTURE},
    {"fn bad<T>(value:T)->i64{const {n}=value;return n}\n"
     "export fn referenceAnswer()->i64{return 42}\n",XR_XIR_BAD_TYPE},
};
static const XrCompileResourceLimits limits = {
    UINT64_C(64)*1024*1024, UINT64_C(8)*1024*1024, UINT64_C(128000000)};

static XrXirStatus compiler_stages(const XrXirCompileContext *context,XrXirSourceResult *source,
    XrCompilerSession **session) {
    XrXirCheckedPacket packet={0};XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    XrXirCSource emitted={0};XrXirStatus status=xr_xir_compile_checked_write(source->checked,&packet,NULL);
    xr_xir_compile_source_result_free(source);xr_compile_session_free(*session);*session=NULL;
    if (status!=XR_XIR_OK) {CHECK(!packet.bytes && !packet.length);goto done;}
    status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL);
    xr_xir_compile_checked_packet_free(&packet);
    if (status!=XR_XIR_OK) {CHECK(!checked);goto done;}
    status=xr_xir_compile_specialize(checked,&closed,NULL);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if (status!=XR_XIR_OK) {CHECK(!closed);goto done;}
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    if (status!=XR_XIR_OK) {CHECK(!lowered);goto done;}
    status=xr_xir_compile_emit_c(lowered,"product_pattern_resources",1048576,&emitted);
    if (status!=XR_XIR_OK) CHECK(!emitted.text && !emitted.length);
    else CHECK(emitted.text && emitted.length);
done:
    xr_xir_compile_c_source_free(&emitted);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_checked_packet_free(&packet);return status;
}

static XrXirStatus pipeline(const SourceCase *test, const XrCompileResourceLimits *bounds, XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL; XrCompilerSession *session = NULL;
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    XrCompileResourceStatus opened = xr_compile_resources_new(bounds, &resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        CHECK(!resources && !physical);
        return opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    }
    XrXirStatus status = XR_XIR_OK;
    XrCompilerSessionStatus session_status = xr_compile_session_new(resources, &session);
    if (session_status != XR_COMPILER_SESSION_OK) {
        status = session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, "type-fi", NULL};
    const XrXirSourceRequest request = {session, NULL, &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    const XrXirSourceText text = {NULL, test->source, strlen(test->source)};
    status = xr_xir_compile_source_check_text(&request, &text, &result, &diagnostic, NULL);
    CHECK(diagnostic.status == status);
    if (status == XR_XIR_OK) {
        CHECK(result.checked && result.snapshot);status=compiler_stages(&context,&result,&session);
    }
    else CHECK(!result.checked && !result.snapshot);
done:
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources); CHECK(!physical);
    return status;
}

static void qualify(const SourceCase *test, unsigned ordinal) {
    XrCompileResourceStats normal = {0}, observed = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(pipeline(test, &limits, &normal) == test->status);
    size_t sites = attempts; CHECK(sites && sites < 20000);
    printf("product compiler case=%u sites=%zu allocated=%llu peak=%llu work=%llu\n", ordinal, sites,
        (unsigned long long)normal.allocated_bytes, (unsigned long long)normal.peak_bytes,
        (unsigned long long)normal.work);
    if (baseline_only) return;
    for (size_t i = 0; i < sites; ++i) {
        attempts = 0; fail_at = i;
        XrXirStatus status = pipeline(test, &limits, &observed);
        if (status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "case=%u site=%zu status=%u\n", ordinal, i, (unsigned)status);
        CHECK(status == XR_XIR_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    XrCompileResourceLimits exact = {normal.allocated_bytes, normal.peak_bytes, normal.work};
    attempts = 0; CHECK(pipeline(test, &exact, &observed) == test->status);
    CHECK(observed.allocated_bytes == normal.allocated_bytes && observed.peak_bytes == normal.peak_bytes && observed.work == normal.work);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits short_limit = exact;
        if (!axis) --short_limit.allocated_bytes;
        else if (axis == 1) --short_limit.live_bytes;
        else --short_limit.work;
        attempts = 0; CHECK(pipeline(test, &short_limit, &observed) == XR_XIR_BUDGET);
    }
    CHECK(!physical);
}

int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--baseline")) || argc==3);
    baseline_only=argc==2;
    if (argc==3) {
        CHECK(!strcmp(argv[1],"--case") && strlen(argv[2])==1 && argv[2][0]>='0' && argv[2][0]<='4');
        unsigned selected=(unsigned)(argv[2][0]-'0');qualify(&cases[selected],selected);
    } else
        for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) qualify(&cases[i],i);
    puts(baseline_only ? "Product compiler baseline measured; exhaustive FI not selected" :
        "Product compiler stages: complete physical FI, exact/minus-one budgets and no partial publication passed");
    return 0;
}
