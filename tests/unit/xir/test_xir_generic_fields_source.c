/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generic_fields_source.c - Owned source Checked execution after producer destruction
 *
 * KEY CONCEPT:
 *   Finite compiler ledgers survive their parser and snapshot producers.
 *   Actual allocation failures cannot publish a partial owned artifact.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while (0)

typedef struct SourceAllocation { void *pointer; size_t bytes; } SourceAllocation;
static SourceAllocation source_allocations[32768];
static size_t source_attempts, source_fail_at=SIZE_MAX, source_live, source_bytes;
static bool source_injected;
static size_t source_slot(const void *pointer) {
    uint64_t key=(uint64_t)(uintptr_t)pointer;
    key^=key>>33; key*=UINT64_C(0xff51afd7ed558ccd); key^=key>>33;
    return (size_t)key&32767;
}
static void *source_malloc(size_t bytes) {
    if (source_attempts++==source_fail_at) { source_injected=true; return NULL; }
    void *pointer=xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(source_live<16384 && bytes<=SIZE_MAX-source_bytes);
    size_t slot=source_slot(pointer);
    while (source_allocations[slot].pointer) slot=(slot+1)&32767;
    source_allocations[slot]=(SourceAllocation){pointer,bytes};
    ++source_live; source_bytes+=bytes;
    return pointer;
}
static void source_free(void *pointer) {
    if (!pointer) return;
    size_t hole=source_slot(pointer);
    while (source_allocations[hole].pointer!=pointer) {
        CHECK(source_allocations[hole].pointer); hole=(hole+1)&32767;
    }
    CHECK(source_live && source_bytes>=source_allocations[hole].bytes);
    --source_live; source_bytes-=source_allocations[hole].bytes;
    for (size_t next=(hole+1)&32767; source_allocations[next].pointer; next=(next+1)&32767) {
        size_t home=source_slot(source_allocations[next].pointer);
        bool stays=hole<=next ? hole<home && home<=next : hole<home || home<=next;
        if (!stays) { source_allocations[hole]=source_allocations[next]; hole=next; }
    }
    source_allocations[hole]=(SourceAllocation){0}; xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) source_malloc(bytes)
#define xr_free(pointer) source_free(pointer)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free

#include "xir_source_fixture_owner.h"
#include "xir_generic_storage_cases.h"
XR_FUNC void xr_test_generic_fields_source_run(XrXirArtifact **checked);
XR_FUNC size_t xr_test_generic_fields_runtime_live(void);
XR_FUNC size_t xr_test_generic_fields_runtime_bytes(void);
static XrCompileResourceStats source_stats(const SourceFixtureOwner *owner) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(owner->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes<=UINT64_C(64)*1024*1024 && stats.peak_bytes<=UINT64_C(8)*1024*1024 &&
        stats.work<=UINT64_C(128000000));
    return stats;
}
static void generic_fields_query_facts(const XrXirSourceResult *result) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(view && view->complete);
    CHECK(xr_xir_compile_artifact_verify(result->checked,NULL)==XR_XIR_OK);
}

int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2); generic_storage_cases();
    CHECK(!source_live && !source_bytes);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    SourceFixtureOwner retained_owner={0}; XrXirArtifact *owned=NULL;
    size_t sites=0,retained_live=0,retained_bytes=0,retained_runtime_live=0,retained_runtime_bytes=0;
    for (size_t pass=0; pass<=sites; ++pass) {
        SourceFixtureOwner failed_owner={0};
        SourceFixtureOwner *owner=pass ? &failed_owner : &retained_owner;
        source_fixture_owner_new(owner);
        XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(owner->context.resources,&session)==XR_COMPILER_SESSION_OK);
        const XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,
            &owner->context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        /* Fault ordinals cover Source Check, including all receiving metadata
         * allocators. Parser session setup is a separate resource boundary. */
        source_attempts=0; source_fail_at=pass ? pass-1 : SIZE_MAX; source_injected=false;
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        size_t attempts=source_attempts; source_fail_at=SIZE_MAX;
        if (!pass) {
            if (status!=XR_XIR_OK) fprintf(stderr,"%u %d %s\n",status,diagnostic.line,diagnostic.message);
            CHECK(status==XR_XIR_OK && result.checked && result.snapshot && !source_injected);
            sites=attempts; CHECK(sites); generic_fields_query_facts(&result);
            XrXirCheckedPacket packet={0};
            CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK && packet.length<=262144);
            if (argc==2) {
                FILE *file=fopen(argv[1],"wb");
                CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
            }
            xr_xir_compile_checked_packet_free(&packet);
            owned=result.checked; result.checked=NULL;
        } else {
            CHECK(source_injected && attempts>pass-1 && status==XR_XIR_OUT_OF_MEMORY &&
                diagnostic.status==XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        }
        xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
        XrCompileResourceStats stats=source_stats(owner);
        if (!pass) {
            CHECK(stats.live_bytes>owner->baseline.live_bytes && owned);
            retained_live=source_live; retained_bytes=source_bytes;
            CHECK(retained_live>1 && retained_bytes>owner->baseline.live_bytes);
            retained_runtime_live=xr_test_generic_fields_runtime_live();
            retained_runtime_bytes=xr_test_generic_fields_runtime_bytes();
        } else {
            CHECK(stats.live_bytes==owner->baseline.live_bytes);
            source_fixture_owner_free(owner);
            CHECK(source_live==retained_live && source_bytes==retained_bytes);
        }
        CHECK(xr_test_generic_fields_runtime_live()==retained_runtime_live &&
            xr_test_generic_fields_runtime_bytes()==retained_runtime_bytes);
    }
    source_fail_at=SIZE_MAX;
    /* Only the detached Checked artifact remains; parser and snapshot owners
     * were destroyed before execution consumes their compiler metadata. */
    xr_test_generic_fields_source_run(&owned); CHECK(!owned);
    CHECK(!xr_test_generic_fields_runtime_live() && !xr_test_generic_fields_runtime_bytes());
    (void)source_stats(&retained_owner); source_fixture_owner_free(&retained_owner);
    CHECK(!source_live && !source_bytes);
    printf("generic fields source %zu actual compiler OOM sites; producer destroyed; compiler physical blocks/bytes=0/0\n",sites);
    return 0;
}
