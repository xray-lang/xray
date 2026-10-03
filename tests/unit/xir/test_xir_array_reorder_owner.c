/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_reorder_owner.c - Source faults and postcommit return ownership
 *
 * KEY CONCEPT:
 *   Method publication precedes fallible ordinary return admission. Physical
 *   allocator indices and cancellable prefixes independently locate that edge.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#if defined(XR_REORDER_NATIVE)
XR_DATA const XrXirProgramSpec reorder_owner_program;
#endif

typedef struct ReorderMetadataBlock { void *pointer; size_t bytes; } ReorderMetadataBlock;
static ReorderMetadataBlock metadata_blocks[4096];
static size_t metadata_attempts, metadata_fail_at=SIZE_MAX;
static uint64_t metadata_live, metadata_bytes, metadata_total, metadata_peak;
static void *physical_malloc(size_t bytes) { return xr_malloc(bytes); }
static void physical_free(void *pointer) { xr_free(pointer); }
static void *metadata_malloc(size_t bytes) {
    if (metadata_attempts++==metadata_fail_at) return NULL;
    void *pointer=xr_malloc(bytes);
    if (!pointer) return NULL;
    size_t i=0; while (i<4096 && metadata_blocks[i].pointer) ++i;
    CHECK(i<4096 && bytes<=UINT64_MAX-metadata_bytes && bytes<=UINT64_MAX-metadata_total);
    metadata_blocks[i]=(ReorderMetadataBlock){pointer,bytes};
    ++metadata_live; metadata_bytes+=bytes; metadata_total+=bytes;
    if (metadata_bytes>metadata_peak) metadata_peak=metadata_bytes;
    return pointer;
}
static void metadata_free(void *pointer) {
    if (!pointer) return;
    size_t i=0; while (i<4096 && metadata_blocks[i].pointer!=pointer) ++i;
    CHECK(i<4096 && metadata_live && metadata_bytes>=metadata_blocks[i].bytes);
    metadata_bytes-=metadata_blocks[i].bytes; --metadata_live;
    metadata_blocks[i]=(ReorderMetadataBlock){0}; xr_free(pointer);
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
#include "xir_runtime_allocations.h"

typedef struct ReorderCompile {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, stats;
    XrXirArtifact *lowered;
    size_t sites;
    XrXirStatus status;
} ReorderCompile;
static XrCompileResourceStats observed_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes==metadata_bytes && stats.allocated_bytes==metadata_total && stats.peak_bytes==metadata_peak);
    return stats;
}
static void metadata_reset(void) {
    CHECK(!metadata_live && !metadata_bytes);
    metadata_attempts=0; metadata_fail_at=SIZE_MAX; metadata_total=metadata_peak=0;
}
/* A complete tiny Source program, rather than a subset copied out of another
 * artifact. Each attempt owns a fresh session and immutable stage outputs. */
static ReorderCompile source_compile(size_t failure, uint64_t work) {
    metadata_reset(); metadata_fail_at=failure;
    ReorderCompile run={0}; run.status=XR_XIR_OUT_OF_MEMORY;
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,work};
    run.context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStatus resource=xr_compile_resources_new(&limits,&run.context.resources);
    if (resource!=XR_COMPILE_RESOURCE_OK) {
        CHECK(resource==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && failure==0 && !run.context.resources);
        run.sites=metadata_attempts; CHECK(run.sites==1 && !metadata_live && !metadata_bytes);
        metadata_fail_at=SIZE_MAX; return run;
    }
    run.baseline=observed_stats(&run.context);
    XrCompilerSession *session=NULL;
    XrXirSourceResult result={0}; XrXirSourceDiagnostic source_diagnostic={0};
    XrXirArtifact *specialized=NULL;
    char *failure_path=NULL;
    XrCompilerSessionStatus session_status=xr_compile_session_new(run.context.resources,&session);
    if (session_status!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);
        CHECK(session_status==XR_COMPILER_SESSION_OUT_OF_MEMORY || session_status==XR_COMPILER_SESSION_BUDGET);
        run.status=session_status==XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REORDER_OWNER_FIXTURES};
    XrXirSourceRequest request={session,XR_REORDER_OWNER_FIXTURES "/root.xr",&authority,&run.context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    run.status=xr_xir_compile_source_check(&request,&result,&source_diagnostic,&failure_path);
    if (run.status!=XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        CHECK(source_diagnostic.status==run.status);
        goto done;
    }
    CHECK(result.checked && result.snapshot);
    CHECK(xr_xir_compile_artifact_context(result.checked)->resources==run.context.resources);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    unsigned reverse=0,unshift=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->native_identity==18) {
            CHECK(!strcmp(decl->name,"reverse") && decl->mutable && !decl->parameter_count);
            CHECK(!strcmp(decl->signature,"() -> Array<T>")); ++reverse;
        }
        if (decl->native_identity==10) {
            CHECK(!strcmp(decl->name,"unshift") && decl->mutable && decl->parameter_count==1);
            CHECK(!strcmp(decl->signature,"(value: T)")); ++unshift;
        }
    }
    CHECK(reverse==1 && unshift==1);
    run.status=xr_xir_compile_specialize(result.checked,&specialized,NULL);
    if (run.status!=XR_XIR_OK) { CHECK(!specialized); goto done; }
    CHECK(xr_xir_compile_artifact_context(specialized)->resources==run.context.resources);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    run.status=xr_xir_compile_lower(specialized,&target,&run.lowered,NULL);
    if (run.status!=XR_XIR_OK) CHECK(!run.lowered);
    else CHECK(xr_xir_compile_artifact_context(run.lowered)->resources==run.context.resources);
done:
    xr_compile_resources_free(failure_path);
    xr_xir_compile_artifact_free(specialized);
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    run.sites=metadata_attempts; metadata_fail_at=SIZE_MAX;
    run.stats=observed_stats(&run.context);
    return run;
}
static void source_release(ReorderCompile *run) {
    xr_xir_compile_artifact_free(run->lowered); run->lowered=NULL;
    if (run->context.resources) {
        CHECK(observed_stats(&run->context).live_bytes==run->baseline.live_bytes);
        xr_compile_resources_release(run->context.resources); run->context.resources=NULL;
    }
    CHECK(!metadata_live && !metadata_bytes);
}
static void source_faults(void) {
    ReorderCompile baseline=source_compile(SIZE_MAX,UINT64_C(128000000));
    CHECK(baseline.status==XR_XIR_OK && baseline.lowered && baseline.sites>1 && baseline.stats.work>1);
    const size_t sites=baseline.sites; const uint64_t work=baseline.stats.work;
    CHECK(baseline.stats.allocated_bytes<=UINT64_C(64)*1024*1024 && baseline.stats.peak_bytes<=UINT64_C(8)*1024*1024);
    source_release(&baseline);
    for (size_t failure=0;failure<sites;++failure) {
        ReorderCompile rejected=source_compile(failure,UINT64_C(128000000));
        if (rejected.status!=XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr,"Source reorder fault=%zu/%zu attempts=%zu status=%u\n",failure,sites,rejected.sites,(unsigned)rejected.status);
        CHECK(rejected.status==XR_XIR_OUT_OF_MEMORY && !rejected.lowered && rejected.sites>failure);
        source_release(&rejected);
    }
    ReorderCompile exact=source_compile(SIZE_MAX,work);
    CHECK(exact.status==XR_XIR_OK && exact.lowered && exact.stats.work==work); source_release(&exact);
    ReorderCompile short_work=source_compile(SIZE_MAX,work-1);
    CHECK(short_work.status==XR_XIR_BUDGET && !short_work.lowered && short_work.stats.work<=work-1);
    source_release(&short_work);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Reorder Source compiler: %zu physical fault sites, work=%llu exact succeeds/minus-one rejects\n",
        sites,(unsigned long long)work);
}

enum {OWNER_ENTRY,OWNER_SAVED,OWNER_STATE,OWNER_REVERSE,OWNER_UNSHIFT,OWNER_FUNCTION_COUNT};
static void owner_functions(const XrXirModule *module,uint32_t *functions) {
    static const char *const names[]={NULL,"saved","state","reverse","unshift"};
    functions[OWNER_ENTRY]=module->declarations->entry_function;
    for (unsigned n=1;n<OWNER_FUNCTION_COUNT;++n) {
        functions[n]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f)
            if (module->functions[f].name_length==strlen(names[n]) &&
                !memcmp(module->functions[f].name,names[n],strlen(names[n]))) functions[n]=f;
        CHECK(functions[n]!=UINT32_MAX);
    }
    for (unsigned n=OWNER_REVERSE;n<=OWNER_UNSHIFT;++n) {
        const XrXirFunction *body=&module->functions[functions[n]];
        unsigned publishes=0;
        for (uint32_t i=0;i<body->instruction_count;++i)
            if (body->instructions[i].op==XR_XIR_PLACE_WRITE) ++publishes;
        CHECK(publishes==1);
    }
}
static XrXirValue owner_run(XrXirInstance *instance,uint32_t function) {
    CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0}; CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    return value;
}
static XrXirInstance *owner_open(XrXirProgram *program,const uint32_t *functions) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL; CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue entry=owner_run(instance,functions[OWNER_ENTRY]);
    CHECK(entry.type==XR_XIR_I64 && !entry.payload); xr_xir_value_drop(&entry); return instance;
}
static void owner_array(const XrXirValue *array,bool committed,bool unshift);
static bool owner_committed(XrXirInstance *instance,const uint32_t *functions,bool unshift) {
    XrXirValue state=owner_run(instance,functions[OWNER_STATE]);
    CHECK(state.type==XR_XIR_I64);
    const int64_t after=unshift ? 7103 : 301;
    CHECK(state.payload==103 || state.payload==after);
    bool committed=state.payload==after; xr_xir_value_drop(&state);
    XrXirValue root=owner_run(instance,functions[OWNER_SAVED]);
    owner_array(&root,committed,unshift); xr_xir_value_drop(&root);
    return committed;
}
static void owner_array(const XrXirValue *array,bool committed,bool unshift) {
    static const int64_t old[]={1,0,3},reversed[]={3,0,1},prepended[]={7,1,0,3};
    const int64_t *expected=committed ? (unshift ? prepended : reversed) : old;
    const int64_t count=committed && unshift ? 4 : 3;
    const int64_t none=committed && unshift ? 2 : 1;
    XrXirDomain *reader=NULL; CHECK(xr_xir_domain_new(65536,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(array),reader,NULL,NULL,10000,65536};
    int64_t length=0; CHECK(xr_xir_array_len(array,&admission,&length)==XR_XIR_VALUE_OK && length==count);
    for (int64_t i=0;i<count;++i) {
        XrXirValue element={0}; XrXirFaultDetail fault={0}; bool some=false; const XrXirValue *payload=NULL;
        CHECK(xr_xir_array_get(array,i,&admission,&element,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&element,&some,&payload) && some==(i!=none));
        if (some) CHECK(payload && payload->type==XR_XIR_I64 && payload->payload==expected[i]);
        else CHECK(!payload);
        xr_xir_value_drop(&element);
    }
    xr_xir_domain_drop(reader);
}
typedef struct OwnerTimeline { size_t ticks,sites,commit; size_t allocation_ticks[4096]; } OwnerTimeline;
static OwnerTimeline owner_timeline(XrXirProgram *program,const uint32_t *functions,bool unshift) {
    OwnerTimeline line={0}; line.commit=SIZE_MAX;
    const uint32_t method=functions[unshift ? OWNER_UNSHIFT : OWNER_REVERSE];
    const size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstance *instance=owner_open(program,functions);
    XrXirValue saved=owner_run(instance,functions[OWNER_SAVED]);
    runtime_attempts=0; runtime_fail_at=SIZE_MAX;
    CHECK(xr_xir_instance_start(instance,method,NULL,0)==XR_XIR_CALL_READY);
    CHECK(runtime_attempts<4096);
    for (size_t i=0;i<runtime_attempts;++i) line.allocation_ticks[i]=0;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    while (status==XR_XIR_CALL_READY) {
        CHECK(line.ticks<512); ++line.ticks;
        size_t first=runtime_attempts;
        status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
        CHECK(runtime_attempts<4096);
        for (size_t i=first;i<runtime_attempts;++i) line.allocation_ticks[i]=line.ticks;
    }
    CHECK(status==XR_XIR_CALL_RETURNED); line.sites=runtime_attempts;
    XrXirValue result={0}; CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    owner_array(&result,true,unshift); xr_xir_value_drop(&result);
    CHECK(owner_committed(instance,functions,unshift));
    owner_array(&saved,false,unshift); xr_xir_value_drop(&saved);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && runtime_live==live && runtime_bytes==bytes);
    /* Locate publication independently: cancelling every real active prefix
     * preserves the module root, so scalar state can observe the first commit. */
    for (size_t prefix=0;prefix<line.ticks;++prefix) {
        instance=owner_open(program,functions);
        saved=owner_run(instance,functions[OWNER_SAVED]);
        CHECK(xr_xir_instance_start(instance,method,NULL,0)==XR_XIR_CALL_READY);
        for (size_t tick=0;tick<prefix;++tick)
            CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        bool committed=owner_committed(instance,functions,unshift);
        if (committed && line.commit==SIZE_MAX) line.commit=prefix;
        CHECK(committed==(line.commit!=SIZE_MAX));
        owner_array(&saved,false,unshift); xr_xir_value_drop(&saved);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && runtime_live==live && runtime_bytes==bytes);
    }
    CHECK(line.commit>=3 && line.commit<line.ticks && line.sites);
    return line;
}
static void owner_return_faults(XrXirProgram *program,const uint32_t *functions,bool unshift) {
    OwnerTimeline line=owner_timeline(program,functions,unshift);
    const size_t live=runtime_live,bytes=runtime_bytes;
    size_t before=0,after=0;
    for (size_t failure=0;failure<line.sites;++failure) {
        XrXirInstance *instance=owner_open(program,functions);
        XrXirValue saved=owner_run(instance,functions[OWNER_SAVED]);
        runtime_attempts=0; runtime_fail_at=failure;
        XrXirCallStatus status=xr_xir_instance_start(instance,functions[unshift ? OWNER_UNSHIFT : OWNER_REVERSE],NULL,0);
        size_t tick=0,failed_tick=runtime_attempts>failure ? 0 : SIZE_MAX;
        while (status==XR_XIR_CALL_READY) {
            CHECK(tick<1024); ++tick;
            size_t first=runtime_attempts;
            status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
            if (first<=failure && failure<runtime_attempts) { CHECK(failed_tick==SIZE_MAX); failed_tick=tick; }
        }
        CHECK(runtime_attempts>failure && status==XR_XIR_CALL_OOM && failed_tick==line.allocation_ticks[failure]);
        runtime_fail_at=SIZE_MAX;
        XrXirValue untouched={0};
        CHECK(xr_xir_instance_take_result(instance,&untouched)==XR_XIR_CALL_BAD_STATE && !untouched.type && !untouched.payload);
        bool postcommit=failed_tick>line.commit;
        CHECK(owner_committed(instance,functions,unshift)==postcommit);
        if (postcommit) ++after; else ++before;
        owner_array(&saved,false,unshift); xr_xir_value_drop(&saved);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && runtime_live==live && runtime_bytes==bytes);
        printf("Reorder %s fault ordinal=%zu tick=%zu commit=%zu root=%s\n",
            unshift ? "unshift" : "reverse",failure,failed_tick,line.commit,postcommit ? "new" : "old");
    }
    CHECK(before && after && before+after==line.sites);
    printf("Reorder %s actual physical OOM: %zu precommit and %zu postcommit return sites, %zu prefix ticks\n",
        unshift ? "unshift" : "reverse",before,after,line.ticks);
}
static void runtime_faults(void) {
    ReorderCompile compiled=source_compile(SIZE_MAX,UINT64_C(128000000));
    CHECK(compiled.status==XR_XIR_OK && compiled.lowered);
    uint32_t functions[OWNER_FUNCTION_COUNT]; owner_functions(xr_xir_compile_artifact_module(compiled.lowered),functions);
    XrXirProgram *program=NULL;
#if defined(XR_REORDER_NATIVE)
    XrXirProgramProof proof=xr_xir_compile_program_proof(compiled.lowered);
    CHECK(proof.length==reorder_owner_program.proof.length);
    CHECK(!memcmp(proof.identity,reorder_owner_program.proof.identity,32));
    CHECK(!memcmp(proof.bytes,reorder_owner_program.proof.bytes,proof.length));
    xr_xir_compile_artifact_free(compiled.lowered); compiled.lowered=NULL;
    CHECK(xr_xir_compile_program_seal(&compiled.context,&reorder_owner_program,&program)==XR_XIR_OK);
#else
    CHECK(xr_xir_compile_vm_program_take(&compiled.lowered,&program)==XR_XIR_OK && !compiled.lowered);
#endif
    owner_return_faults(program,functions,false); owner_return_faults(program,functions,true);
    XrXirInstance *instance=owner_open(program,functions);
    XrXirValue saved=owner_run(instance,functions[OWNER_SAVED]);
    XrXirValue escaped=owner_run(instance,functions[OWNER_REVERSE]);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(observed_stats(&compiled.context).live_bytes>compiled.baseline.live_bytes);
    owner_array(&saved,false,false); owner_array(&escaped,true,false);
    xr_xir_value_drop(&saved); xr_xir_value_drop(&escaped);
    CHECK(!runtime_live && !runtime_bytes);
    source_release(&compiled);
    puts("Reorder nullable results survive Instance/Program release, then physical ownership reaches zero");
}
static void emit_owner(const char *path) {
    ReorderCompile compiled=source_compile(SIZE_MAX,UINT64_C(128000000));
    CHECK(compiled.status==XR_XIR_OK && compiled.lowered);
    XrXirCSource source={0};
    CHECK(xr_xir_compile_emit_c(compiled.lowered,"reorder_owner",1048576,&source)==XR_XIR_OK);
    FILE *file=fopen(path,"wb"); CHECK(file);
    CHECK(fwrite(source.text,1,source.length,file)==source.length && fclose(file)==0);
    printf("Reorder owner generated C bytes=%zu (cap1048576), verified before native compilation\n",source.length);
    xr_xir_compile_c_source_free(&source);
    source_release(&compiled);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);
    if (argc==3) { CHECK(!strcmp(argv[1],"emit")); emit_owner(argv[2]); }
    else if (!strcmp(argv[1],"compiler")) source_faults();
    else { CHECK(!strcmp(argv[1],"runtime")); runtime_faults(); }
    CHECK(!metadata_live && !metadata_bytes && !runtime_live && !runtime_bytes);
    return 0;
}
