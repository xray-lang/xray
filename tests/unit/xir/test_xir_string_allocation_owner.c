/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_allocation_owner.c - Independent bytes and actual allocation failures
 *
 * KEY CONCEPT:
 *   Runtime and compiler allocations have separate fault indices and physical
 *   observers; escaped Error metadata retains the same bounded compile ledger.
 */
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)

typedef struct MetadataBlock { void *pointer; size_t bytes; } MetadataBlock;
static MetadataBlock metadata_blocks[4096];
static size_t metadata_attempts, metadata_fail_at=SIZE_MAX;
static uint64_t metadata_live, metadata_bytes, metadata_total, metadata_peak;
/* Capture the host allocator before temporarily observing resource metadata. */
static void *physical_malloc(size_t bytes) { return xr_malloc(bytes); }
static void physical_free(void *pointer) { xr_free(pointer); }
static void *metadata_malloc(size_t bytes) {
    if (metadata_attempts++==metadata_fail_at) return NULL;
    void *pointer=xr_malloc(bytes);
    if (!pointer) return NULL;
    size_t i=0; while (i<4096 && metadata_blocks[i].pointer) ++i;
    CHECK(i<4096 && bytes<=UINT64_MAX-metadata_bytes && bytes<=UINT64_MAX-metadata_total);
    metadata_blocks[i]=(MetadataBlock){pointer,bytes};
    ++metadata_live; metadata_bytes+=bytes; metadata_total+=bytes;
    if (metadata_bytes>metadata_peak) metadata_peak=metadata_bytes;
    return pointer;
}
static void metadata_free(void *pointer) {
    if (!pointer) return;
    size_t i=0; while (i<4096 && metadata_blocks[i].pointer!=pointer) ++i;
    CHECK(i<4096 && metadata_live && metadata_bytes>=metadata_blocks[i].bytes);
    metadata_bytes-=metadata_blocks[i].bytes; --metadata_live;
    metadata_blocks[i]=(MetadataBlock){0}; xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) metadata_malloc(bytes)
#define xr_free(pointer) metadata_free(pointer)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) physical_malloc(bytes)
#define xr_free(pointer) physical_free(pointer)

/* The production runtime bodies allocate through the independent observer. */
#include "xir_runtime_allocations.h"
#include "xir_source_fixture_owner.h"
#include "xir_string_fixture.h"

static const char left_bytes[]="A\0\xE4\xB8\xAD";
static const char right_bytes[]="\xF0\x9F\x98\x80!";
static const char expected_bytes[]="A\0\xE4\xB8\xAD\xF0\x9F\x98\x80!";
typedef struct StringProbe { XrXirCall *call; unsigned mode, outputs; } StringProbe;
typedef struct StringRun { size_t sites; XrXirCallStatus status; XrXirValue escaped; } StringRun;

static void bytes_equal(const XrXirValue *value) {
    const char *bytes=NULL; size_t count=0;
    CHECK(xr_xir_string_view(value,&bytes,&count));
    CHECK(count==sizeof(expected_bytes)-1 && !memcmp(bytes,expected_bytes,count));
}
static void error_bytes_equal(const XrXirValue *value, XrXirDomain *domain) {
    uint32_t variant=UINT32_MAX;
    CHECK(xr_xir_enum_variant(value,&variant)==XR_XIR_VALUE_OK && variant==1);
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,10000,65536};
    XrXirValue field={0};
    CHECK(xr_xir_enum_get(value,1,0,&admission,&field)==XR_XIR_VALUE_OK);
    bytes_equal(&field); xr_xir_value_drop(&field);
}
static XrXirOutputStatus publish_string(void *context, const XrXirOutputGroup *group) {
    StringProbe *probe=context;
    CHECK(group && !group->line && group->count==1);
    CHECK(group->stream==(probe->mode==5 || probe->mode==6 || probe->outputs ? XR_XIR_STDOUT : XR_XIR_STDERR));
    bytes_equal(&group->values[0]); ++probe->outputs;
    XrXirValue untouched={0};
    CHECK(xr_xir_call_poll_bounded(probe->call,UINT64_MAX).status==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_call_take_result(probe->call,&untouched)==XR_XIR_CALL_BUSY && !untouched.type);
    CHECK(xr_xir_call_free(probe->call)==XR_XIR_CALL_BUSY);
    if (probe->mode==6 && probe->outputs==3)
        CHECK(xr_xir_call_request_cancel(probe->call)==XR_XIR_CALL_CANCEL_REQUESTED);
    return probe->mode==3 ? XR_XIR_OUTPUT_ERROR : XR_XIR_OUTPUT_OK;
}
static XrCompileResourceStats observed_stats(const SourceFixtureOwner *owner) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(owner->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==metadata_bytes && stats.allocated_bytes==metadata_total && stats.peak_bytes==metadata_peak);
    return stats;
}
static void reset_metadata(void) {
    CHECK(!metadata_live && !metadata_bytes);
    metadata_attempts=0; metadata_fail_at=SIZE_MAX; metadata_total=metadata_peak=0;
}
static XrXirCallStatus expected_status(unsigned variant, unsigned mode) {
    if (mode==1 || mode==6) return XR_XIR_CALL_CANCELLED;
    if (mode==2) return XR_XIR_CALL_SUSPENDED;
    if (mode==3 || mode==4) return XR_XIR_CALL_OUTPUT_ERROR;
    if (mode==5 || mode==8) return XR_XIR_CALL_LIMIT;
    return variant ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED;
}

/* Preserve the original managed-string allocation scan's order: bind, domain,
 * arena, argument owners, activation and polls, then release every partial owner. */
static StringRun managed_run(const SourceFixtureOwner *owner, XrXirArtifact *artifact,
    unsigned variant, unsigned mode, bool escape) {
    XrXirDomain *domain=NULL; XrXirTypeArena *arena=NULL;
    XrXirValue arguments[2]={{0},{0}}, owned={0}; XrXirCall *call=NULL;
    XrXirCallEntry entries[3]; XrXirVmBinding bindings[3];
    XrXirCallAccounting accounting={0}; XrXirCallConfig config;
    StringProbe probe={NULL,mode,0}; StringRun run={0,XR_XIR_CALL_OOM,{0}};
    bool injected=runtime_fail_at!=SIZE_MAX;
    XrCompileResourceStats metadata_base=observed_stats(owner);
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries; config.entry_count=3; config.byte_limit=65536;
    config.poll_limit=mode==5 ? 15 : 100; config.depth_limit=10; config.accounting=&accounting;
    config.output=mode==4 ? (XrXirOutputProvider){0} :
        (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,publish_string,&probe};
    for (uint32_t i=0;i<3;++i) {
        XrXirStatus status=xr_xir_compile_vm_bind(artifact,i,&bindings[i],&entries[i]);
        CHECK(status==XR_XIR_OK);
    }
    XrXirValueStatus value_status=xr_xir_domain_new(mode==8 ? 240 : 65536,&domain);
    if (value_status!=XR_XIR_VALUE_OK) { CHECK(value_status==XR_XIR_VALUE_OOM && injected); goto done; }
    CHECK(error_fixture_arena(&owner->context,&arena)==XR_XIR_VALUE_OK);
    config.admission=error_fixture_admission(domain,arena);
    value_status=xr_xir_string_new(domain,left_bytes,sizeof(left_bytes)-1,&arguments[0]);
    if (value_status!=XR_XIR_VALUE_OK) { CHECK(value_status==XR_XIR_VALUE_OOM && injected); goto done; }
    value_status=xr_xir_string_new(domain,right_bytes,sizeof(right_bytes)-1,&arguments[1]);
    if (value_status!=XR_XIR_VALUE_OK) { CHECK(value_status==XR_XIR_VALUE_OOM && injected); goto done; }
    XrXirCallStatus admitted=xr_xir_call_new(&config,mode==5 || mode==6 ? 2 : 0,arguments,2,&call);
    if (admitted!=XR_XIR_CALL_READY) { CHECK(admitted==XR_XIR_CALL_OOM && injected); goto done; }
    probe.call=call;
    xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]);
    CHECK(xr_xir_call_take_result(call,&owned)==XR_XIR_CALL_BAD_STATE);
    XrXirCallResult result=xr_xir_call_poll_bounded(call,UINT64_MAX);
    if (!injected && (mode==0 || mode==1 || mode==2 || mode==7))
        CHECK(result.status==XR_XIR_CALL_SUSPENDED && probe.outputs==1);
    if (result.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(probe.outputs==1 && xr_xir_call_take_result(call,&owned)==XR_XIR_CALL_BAD_STATE);
        if (mode!=2) {
            if (mode==1) CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
            else CHECK(xr_xir_call_resume(call,result.wake)==XR_XIR_CALL_READY);
            result=xr_xir_call_poll_bounded(call,UINT64_MAX);
        }
    }
    run.status=result.status;
    CHECK(run.status==(injected ? XR_XIR_CALL_OOM : expected_status(variant,mode)));
    run.sites=runtime_attempts; runtime_fail_at=SIZE_MAX;
    if (run.status==XR_XIR_CALL_RETURNED || run.status==XR_XIR_CALL_THROWN) {
        if (variant) error_bytes_equal(&result.value,domain); else bytes_equal(&result.value);
        if (mode!=7) {
            CHECK(xr_xir_call_take_result(call,&owned)==run.status);
            CHECK(xr_xir_call_poll_bounded(call,UINT64_MAX).status==XR_XIR_CALL_CONSUMED);
            XrXirValue second={0};
            CHECK(xr_xir_call_take_result(call,&second)==XR_XIR_CALL_BAD_STATE && !second.type);
        }
    }
    if (!injected) {
        if (mode==0 || mode==7) CHECK(probe.outputs==(variant ? 1u : 2u));
        if (mode==1 || mode==2 || mode==3) CHECK(probe.outputs==1);
        if (mode==4 || mode==8) CHECK(!probe.outputs);
        if (mode==5) CHECK(probe.outputs && probe.outputs<15);
        if (mode==6) CHECK(probe.outputs==3);
    }
done:
    /* Independent reader allocations are oracle work, outside the failing
     * activation's measured allocation set; physical accounting still sees them. */
    if (!run.sites) run.sites=runtime_attempts;
    runtime_fail_at=SIZE_MAX;
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && !accounting.depth && accounting.allocations==accounting.frees);
    xr_xir_compile_type_arena_drop(arena);
    xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]); xr_xir_domain_drop(domain);
    if (owned.type && owned.type!=XR_XIR_STRING) {
        XrCompileResourceStats held=observed_stats(owner);
        CHECK(held.live_bytes>metadata_base.live_bytes);
        XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
        error_bytes_equal(&owned,reader); xr_xir_domain_drop(reader);
    } else if (owned.type==XR_XIR_STRING) bytes_equal(&owned);
    if (escape) run.escaped=owned;
    else {
        xr_xir_value_drop(&owned);
        CHECK(observed_stats(owner).live_bytes==metadata_base.live_bytes);
        CHECK(!runtime_live && !runtime_bytes);
    }
    return run;
}
static void runtime_allocation_failures(void) {
    for (unsigned variant=0;variant<2;++variant) for (unsigned mode=0;mode<9;++mode) {
        CHECK(!runtime_live && !runtime_bytes); reset_metadata();
        SourceFixtureOwner owner={0}; source_fixture_owner_new(&owner);
        XrXirArtifact *artifact=string_fixture(&owner.context,variant);
        runtime_fail_at=SIZE_MAX; runtime_attempts=0;
        StringRun baseline=managed_run(&owner,artifact,variant,mode,false);
        CHECK(baseline.sites && baseline.status==expected_status(variant,mode));
        for (size_t failure=0;failure<baseline.sites;++failure) {
            runtime_fail_at=failure; runtime_attempts=0;
            StringRun rejected=managed_run(&owner,artifact,variant,mode,false);
            CHECK(rejected.status==XR_XIR_CALL_OOM && rejected.sites>failure);
        }
        xr_xir_compile_artifact_free(artifact); source_fixture_owner_free(&owner);
        CHECK(!metadata_live && !metadata_bytes && !runtime_live && !runtime_bytes);
        printf("String runtime physical OOM: variant=%u mode=%u actual sites=%zu\n",variant,mode,baseline.sites);
    }
}
static void escaped_result_after_artifact(void) {
    for (unsigned variant=0;variant<2;++variant) {
        reset_metadata(); SourceFixtureOwner owner={0}; source_fixture_owner_new(&owner);
        XrXirArtifact *artifact=string_fixture(&owner.context,variant);
        runtime_fail_at=SIZE_MAX; runtime_attempts=0;
        StringRun run=managed_run(&owner,artifact,variant,0,true);
        CHECK(run.escaped.type && runtime_live && runtime_bytes);
        xr_xir_compile_artifact_free(artifact);
        XrCompileResourceStats held=observed_stats(&owner);
        CHECK(variant ? held.live_bytes>owner.baseline.live_bytes : held.live_bytes==owner.baseline.live_bytes);
        if (variant) {
            XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
            error_bytes_equal(&run.escaped,reader); xr_xir_domain_drop(reader);
        } else bytes_equal(&run.escaped);
        xr_xir_value_drop(&run.escaped);
        CHECK(!runtime_live && !runtime_bytes);
        source_fixture_owner_free(&owner); CHECK(!metadata_live && !metadata_bytes);
    }
}
/* This is a separate actual metadata fault scan, rather than attributing
 * compile-resource allocations to the runtime wrapper's old fault indices. */
static size_t metadata_arena_run(size_t failure) {
    reset_metadata(); metadata_fail_at=failure;
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus status=xr_compile_resources_new(&limits,&context.resources);
    if (status!=XR_COMPILE_RESOURCE_OK) {
        CHECK(failure==0 && status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources);
        CHECK(metadata_attempts==1 && !metadata_live && !metadata_bytes); return metadata_attempts;
    }
    XrXirTypeArena *arena=NULL;
    XrXirValueStatus admitted=error_fixture_arena(&context,&arena);
    size_t sites=metadata_attempts;
    if (failure!=SIZE_MAX) {
        CHECK(admitted==XR_XIR_VALUE_OOM && !arena && sites>failure);
        xr_compile_resources_release(context.resources);
    } else {
        CHECK(admitted==XR_XIR_VALUE_OK && arena && sites>1);
        XrXirDomain *domain=NULL; CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
        XrXirValue text={0},escaped={0};
        CHECK(xr_xir_string_new(domain,expected_bytes,sizeof(expected_bytes)-1,&text)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission=error_fixture_admission(domain,arena);
        CHECK(xr_xir_enum_new((XrXirType)256,1,&text,1,&admission,&escaped)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&text); xr_xir_compile_type_arena_drop(arena);
        xr_compile_resources_release(context.resources); xr_xir_domain_drop(domain);
        CHECK(metadata_live && metadata_bytes && xr_xir_value_valid(&escaped));
        XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
        error_bytes_equal(&escaped,reader); xr_xir_domain_drop(reader); xr_xir_value_drop(&escaped);
    }
    CHECK(!metadata_live && !metadata_bytes && !runtime_live && !runtime_bytes);
    metadata_fail_at=SIZE_MAX; return sites;
}
static void metadata_allocation_failures(void) {
    runtime_fail_at=SIZE_MAX; runtime_attempts=0;
    size_t sites=metadata_arena_run(SIZE_MAX);
    for (size_t failure=0;failure<sites;++failure) metadata_arena_run(failure);
    printf("String Error metadata actual allocation OOM: ledger+arena sites=%zu\n",sites);
}
int main(void) {
    runtime_allocation_failures(); escaped_result_after_artifact(); metadata_allocation_failures();
    CHECK(!metadata_live && !metadata_bytes && !runtime_live && !runtime_bytes);
    puts("Independent string bytes, runtime and metadata OOM, cancellation and escaped Error physical zero");
    return 0;
}
