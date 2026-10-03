/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cgen_verify_allocations.c - Actual verifier failure and release witnesses
 *
 * KEY CONCEPT:
 *   Instrument the real verifier and exercise both XIR emission boundaries.
 */
#include "base/xmalloc.h"
#include "../xir/xir_execution_fixture.h"
#include "../xir/xir_source_fixture_owner.h"
#include "../xir/xir_call_fixture.h"
#include "xir/xxir_emit_c.h"
#include "aot/xi_cgen_verify_output.h"
#include <stdint.h>

typedef struct Allocation {void *pointer;size_t bytes;} Allocation;
static Allocation allocations[2];
static size_t calls, live, bytes_live, fail_at=SIZE_MAX;
static void *counted_malloc(size_t size) {
    if (calls++==fail_at) return NULL;
    void *pointer=xr_malloc(size);
    if (pointer) {CHECK(live<2);allocations[live++]=(Allocation){pointer,size};bytes_live+=size;}
    return pointer;
}
static void *counted_realloc(void *pointer,size_t size) {
    if (calls++==fail_at) return NULL;
    size_t index=0;while (index<live && allocations[index].pointer!=pointer) ++index;
    bool fresh=pointer==NULL;CHECK(fresh || index<live);
    void *grown=xr_realloc(pointer,size);
    if (grown) {
        if (fresh) {CHECK(live<2);index=live++;}
        else bytes_live-=allocations[index].bytes;
        allocations[index]=(Allocation){grown,size};bytes_live+=size;
    }
    return grown;
}
static void counted_free(void *pointer) {
    if (pointer) {
        size_t index=0;while (index<live && allocations[index].pointer!=pointer) ++index;
        CHECK(index<live);bytes_live-=allocations[index].bytes;allocations[index]=allocations[--live];
    }
    xr_free(pointer);
}
#undef xr_malloc
#undef xr_realloc
#undef xr_free
#define xr_malloc counted_malloc
#define xr_realloc counted_realloc
#define xr_free counted_free
#include "../../../src/aot/xi_cgen_verify_output.c"
#undef xr_malloc
#undef xr_realloc
#undef xr_free

static void allocation_scan(const char *source) {
    calls=0;fail_at=SIZE_MAX;XiCgenVerifyResult result;
    CHECK(xi_cgen_verify_output(source,strlen(source),&result)==XI_CGEN_VERIFY_PASSED);
    CHECK(result.category==XI_CGEN_VERIFY_OK && !live && !bytes_live);
    size_t total=calls;CHECK(total>=3);
    for (size_t point=0;point<total;++point) {
        calls=0;fail_at=point;memset(&result,0xa5,sizeof(result));XiCgenVerifyResult saved=result;
        CHECK(xi_cgen_verify_output(source,strlen(source),&result)==XI_CGEN_VERIFY_OUT_OF_MEMORY);
        CHECK(!memcmp(&result,&saved,sizeof(result)) && !live && !bytes_live);
        calls=0;
        CHECK(xi_cgen_verify_output_or_ice(source,strlen(source),"allocation-test")==XI_CGEN_VERIFY_OUT_OF_MEMORY);
        CHECK(!live && !bytes_live);
    }
    printf("actual verifier allocation points=%zu physical=0\n",total);fail_at=SIZE_MAX;
}
static void emission_scan(const XrXirArtifact *artifact,bool resumable) {
    fail_at=SIZE_MAX;calls=0;XrXirCSource output={0};
    XrXirStatus status=resumable ? xr_xir_emit_c(artifact,"resource_test",1048576,&output) :
        xr_xir_emit_leaf_c(artifact,"resource_test",1048576,&output);
    CHECK(status==XR_XIR_OK && output.text && !live && !bytes_live);
    size_t total=calls;CHECK(total>=1);xr_xir_c_source_free(&output);
    for (size_t point=0;point<total;++point) {
        fail_at=point;calls=0;output=(XrXirCSource){NULL,0};XrXirCSource saved=output;
        status=resumable ? xr_xir_emit_c(artifact,"resource_test",1048576,&output) :
            xr_xir_emit_leaf_c(artifact,"resource_test",1048576,&output);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !memcmp(&output,&saved,sizeof(output)) && !live && !bytes_live);
    }
    printf("actual %s emitter verifier points=%zu physical=0\n",resumable ? "resumable" : "leaf",total);fail_at=SIZE_MAX;
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--ice")) {
        const char malformed[]="void f(void) {\n";
        (void)xi_cgen_verify_output_or_ice(malformed,sizeof(malformed)-1,"malformed-test");
        return 1;
    }
    CHECK(argc==1);
    const char body[]="void f(void) {\n int v0=1;\n int v2048=2;\n int v4096=3;\n return;\n}\n";
    const char macros[]="#define v0 0\n#define v2048 2\n#define v4096 3\nvoid f(void) {\n}\n";
    allocation_scan(body);allocation_scan(macros);
    XiCgenVerifyResult result;memset(&result,0xcc,sizeof(result));XiCgenVerifyResult saved=result;
    calls=0;CHECK(xi_cgen_verify_output(NULL,1,&result)==XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(!calls && !memcmp(&result,&saved,sizeof(result)));
    CHECK(xi_cgen_verify_output_or_ice(NULL,1,"invalid")==XI_CGEN_VERIFY_BAD_ARGUMENT);
    CHECK(xi_cgen_verify_output(NULL,0,&result)==XI_CGEN_VERIFY_PASSED);
    XiCgenVerifyResult zero={0};CHECK(!memcmp(&result,&zero,sizeof(result)));
    CHECK(xi_cgen_verify_output(body,sizeof(body)-1,NULL)==XI_CGEN_VERIFY_PASSED && !live && !bytes_live);
    XrXirArtifact *artifact=fixture_lowered();emission_scan(artifact,false);xr_xir_artifact_free(artifact);
    artifact=uninitialized_leaf_fixture();emission_scan(artifact,false);xr_xir_artifact_free(artifact);
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    artifact=call_fixture(&owner.context,0);emission_scan(artifact,true);xr_xir_compile_artifact_free(artifact);
    source_fixture_owner_free(&owner);
    puts("typed verifier OOM, unchanged outputs and physical release PASS");return 0;
}
