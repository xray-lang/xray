/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_compile_owner.h - Finite complete source fixture operations
 */
#ifndef XIR_SOURCE_COMPILE_OWNER_H
#define XIR_SOURCE_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include <string.h>
typedef struct SourceFixtureCompileAllocation { void *pointer; size_t bytes; } SourceFixtureCompileAllocation;
static SourceFixtureCompileAllocation source_fixture_compile_allocations[65536];
static size_t source_fixture_compile_live, source_fixture_compile_bytes, source_fixture_compile_attempts;
static size_t source_fixture_compile_peak_blocks,source_fixture_compile_peak_bytes;
static const char *source_fixture_compile_phase="ledger",*source_fixture_compile_peak_phase="ledger";
static size_t source_fixture_compile_fail_at=SIZE_MAX;
static bool source_fixture_compile_injected;
static size_t source_fixture_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 65535;
}
static void *source_fixture_compile_malloc(size_t bytes) {
    if (source_fixture_compile_attempts++==source_fixture_compile_fail_at) {
        source_fixture_compile_injected=true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    if(source_fixture_compile_live>=32768)fprintf(stderr,"Source compiler observer full: blocks=%zu bytes=%zu attempts=%zu\n",source_fixture_compile_live,source_fixture_compile_bytes,source_fixture_compile_attempts);
    CHECK(source_fixture_compile_live < 32768 && bytes <= SIZE_MAX - source_fixture_compile_bytes);
    size_t slot = source_fixture_compile_slot(pointer);
    while (source_fixture_compile_allocations[slot].pointer) slot = (slot + 1) & 65535;
    source_fixture_compile_allocations[slot] = (SourceFixtureCompileAllocation){pointer, bytes};
    ++source_fixture_compile_live; source_fixture_compile_bytes += bytes;
    if(source_fixture_compile_live>source_fixture_compile_peak_blocks){source_fixture_compile_peak_blocks=source_fixture_compile_live;source_fixture_compile_peak_phase=source_fixture_compile_phase;}
    if(source_fixture_compile_bytes>source_fixture_compile_peak_bytes)source_fixture_compile_peak_bytes=source_fixture_compile_bytes;
    return pointer;
}
static void source_fixture_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = source_fixture_compile_slot(pointer);
    while (source_fixture_compile_allocations[hole].pointer != pointer) {
        CHECK(source_fixture_compile_allocations[hole].pointer); hole = (hole + 1) & 65535;
    }
    CHECK(source_fixture_compile_live && source_fixture_compile_bytes >= source_fixture_compile_allocations[hole].bytes);
    --source_fixture_compile_live; source_fixture_compile_bytes -= source_fixture_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & 65535; source_fixture_compile_allocations[next].pointer; next = (next + 1) & 65535) {
        size_t home = source_fixture_compile_slot(source_fixture_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { source_fixture_compile_allocations[hole] = source_fixture_compile_allocations[next]; hole = next; }
    }
    source_fixture_compile_allocations[hole] = (SourceFixtureCompileAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) source_fixture_compile_malloc(bytes)
#define xr_free(pointer) source_fixture_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

typedef struct SourceFixtureSourceOwner { XrXirCompileContext context; XrCompileResourceStats baseline; } SourceFixtureSourceOwner;
static SourceFixtureSourceOwner source_fixture_source_owners[512];
static size_t source_fixture_source_owner_count;
/* Separate semantic probes own separate finite graphs; all transitions of a
 * given graph derive the original artifact context and retain its ledger. */
static inline const XrXirCompileContext *source_fixture_source_owner(uint64_t allocated, uint64_t work) {
    CHECK(source_fixture_source_owner_count < 512);
    CHECK(allocated <= UINT64_C(64)*1024*1024 && work <= UINT64_C(128000000));
    SourceFixtureSourceOwner *o=&source_fixture_source_owners[source_fixture_source_owner_count++];
    XrCompileResourceLimits caps={allocated,UINT64_C(8)*1024*1024,work};
    CHECK(xr_compile_resources_new(&caps,&o->context.resources)==XR_COMPILE_RESOURCE_OK);
    o->context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(o->context.resources,&o->baseline)==XR_COMPILE_RESOURCE_OK);
    return &o->context;
}
static inline void source_fixture_source_owner_close(const XrXirCompileContext *context) {
    for(size_t i=0;i<source_fixture_source_owner_count;++i){SourceFixtureSourceOwner *owner=&source_fixture_source_owners[i];
        if(owner->context.resources!=context->resources)continue;
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.live_bytes==owner->baseline.live_bytes);
        fprintf(stderr,"Source closed negative owner: allocated=%llu peak=%llu work=%llu live=%llu\n",
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,(unsigned long long)stats.live_bytes);
        xr_compile_resources_release(owner->context.resources);*owner=(SourceFixtureSourceOwner){0};return;}
    CHECK(false);
}
static inline void source_fixture_source_owners_free(void) {
    uint64_t max_allocated=0,max_peak=0,max_work=0;
    for(size_t i=0;i<source_fixture_source_owner_count;++i) {
        SourceFixtureSourceOwner *o=&source_fixture_source_owners[i];if(!o->context.resources)continue;XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(o->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.live_bytes==o->baseline.live_bytes);
        if(stats.allocated_bytes>max_allocated)max_allocated=stats.allocated_bytes;
        if(stats.peak_bytes>max_peak)max_peak=stats.peak_bytes;
        if(stats.work>max_work)max_work=stats.work;
        xr_compile_resources_release(o->context.resources);*o=(SourceFixtureSourceOwner){0};
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"effects Source compiler: %zu finite owners, max allocated=%llu peak=%llu work=%llu; physical=0/0\n",
        source_fixture_source_owner_count,(unsigned long long)max_allocated,(unsigned long long)max_peak,(unsigned long long)max_work);
    fprintf(stderr,"Source compiler physical peak: blocks=%zu bytes=%zu phase=%s; final=0/0\n",source_fixture_compile_peak_blocks,source_fixture_compile_peak_bytes,source_fixture_compile_peak_phase);
    source_fixture_source_owner_count=0;
}

#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "toolchain/xcompiler_session.h"
#if defined(XR_SOURCE_FIXTURES)
static XrXirArtifact *source_fixture_lower(const XrXirCompileContext *context,const char *packet_path) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK && session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    source_fixture_compile_phase="Source check";
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"source %u:%d:%d: %s (%u)\n",diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message,(unsigned)status);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *checked=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};
    source_fixture_compile_phase="Checked write";
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file);
        CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && fclose(file)==0);}
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    source_fixture_compile_phase="Checked specialize";
    XrXirDiagnostic specialize_diagnostic={0};
    status=xr_xir_compile_specialize(checked,&specialized,&specialize_diagnostic);
    if(status!=XR_XIR_OK){XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Source specialize status=%u function=%u block=%u instruction=%u allocated=%llu peak=%llu work=%llu live=%llu\n",
            (unsigned)status,specialize_diagnostic.function,specialize_diagnostic.block,specialize_diagnostic.instruction,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,
            (unsigned long long)stats.work,(unsigned long long)stats.live_bytes);
        const XrXirModule *failed=xr_xir_compile_artifact_module(checked);
        if(specialize_diagnostic.function<failed->function_count){
            const XrXirFunction *fn=&failed->functions[specialize_diagnostic.function];
            fprintf(stderr,"Source before specialize parameters=%u instructions=%u operands=%u\n",
                fn->parameter_count,fn->instruction_count,fn->operand_count);
            if(specialize_diagnostic.instruction<fn->instruction_count){
                const XrXirInstruction *op=&fn->instructions[specialize_diagnostic.instruction];
                fprintf(stderr,"Source before opcode=%u type=%u args=%u/%u target=%u/%u immediate=%lld types=%u/%u\n",
                    (unsigned)op->op,(unsigned)op->type,op->args[0],op->args[1],op->targets[0],op->targets[1],
                    (long long)op->immediate,op->type_arguments[0],op->type_arguments[1]);}}}
    CHECK(status==XR_XIR_OK && specialized);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirDiagnostic lower_diagnostic={0};source_fixture_compile_phase="Lower";status=xr_xir_compile_lower(specialized,&target,&lowered,&lower_diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"lower status %u function %u block %u instruction %u\n",(unsigned)status,lower_diagnostic.function,lower_diagnostic.block,lower_diagnostic.instruction);
    CHECK(status==XR_XIR_OK && lowered);xr_xir_compile_artifact_free(specialized);return lowered;
}
#endif
#endif // XIR_SOURCE_COMPILE_OWNER_H
