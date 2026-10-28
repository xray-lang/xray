/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_compiler_fault_cases.h - Ten-stage compiler histories and owned resource axes
 */
#ifndef XIR_GENERATOR_COMPILER_FAULT_CASES_H
#define XIR_GENERATOR_COMPILER_FAULT_CASES_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xxir.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
static size_t source_program_compile_attempts;
static const size_t *receiver_failure_plan;
static size_t receiver_failure_count,receiver_failure_next;
/* The existing compiler probe still owns the original ordinal and primary
 * failure. Only its real malloc leaf observes additional failure-history
 * ordinals; observer storage and every runtime allocation remain outside. */
static void *receiver_compile_leaf_malloc(size_t bytes) {
    size_t ordinal=source_program_compile_attempts-1;
    if(receiver_failure_plan && receiver_failure_next<receiver_failure_count &&
        receiver_failure_plan[receiver_failure_next]==ordinal) {++receiver_failure_next;return NULL;}
    return xr_malloc(bytes);
}
#pragma push_macro("xr_malloc")
#undef xr_malloc
#define xr_malloc(bytes) receiver_compile_leaf_malloc(bytes)
#include "xir_library_compile_owner.h"
#pragma pop_macro("xr_malloc")
/* Failure-tree storage belongs to the observer, outside both compiler and
 * runtime accounting. Per-history physical-zero checks remain unconditional. */
static void *receiver_history_storage(size_t bytes) {return xr_malloc(bytes);}
static void receiver_history_storage_free(void *pointer) {xr_free(pointer);}
#include "xir_runtime_allocations.h"
#include "base/xsha256.h"
static unsigned receiver_compile_calls;
static const char *receiver_compile_stdlib=XR_SOURCE_STDLIB;
typedef struct ReceiverCompileResult {XrXirProgram *program;uint32_t entry;bool keep;} ReceiverCompileResult;
static uint32_t receiver_compile_entry(const XrXirModule *module);
static void receiver_compile_oracle(XrXirProgram *program,uint32_t entry);

static XrXirStatus receiver_compile_operation(const XrXirCompileContext *context,void *opaque) {
    ReceiverCompileResult *result=opaque;CHECK(result && !result->program);
    XrCompilerSession *session=NULL;XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirCheckedPacket first={0},second={0};XrXirArtifact *read=NULL,*closed=NULL,*reloaded=NULL,*lowered=NULL;
    XrXirCSource emitted={0};XrXirProgram *program=NULL;char *failure=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    CHECK(opened==XR_COMPILER_SESSION_OK || opened==XR_COMPILER_SESSION_BUDGET || opened==XR_COMPILER_SESSION_OUT_OF_MEMORY);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status!=XR_XIR_OK) {CHECK(!session);return status;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"receiver-compiler",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,receiver_compile_stdlib,NULL,XR_XIR_PROGRAM,NULL};
    ++receiver_compile_calls;
    status=xr_xir_compile_source_check_text(&request,
        &(XrXirSourceText){NULL,receiver_compile_source,sizeof(receiver_compile_source)-1},&source,&diagnostic,&failure);
    xr_compile_session_free(session);session=NULL;
    if(status!=XR_XIR_OK) {
        CHECK(!source.checked && !source.snapshot);
        if(source_program_compile_fail_at==SIZE_MAX)
            fprintf(stderr,"Receiver Source status%u line%d: %s\n",status,diagnostic.line,diagnostic.message);
        goto done;
    }
    CHECK(source.checked && source.snapshot && !failure);
    ++receiver_compile_calls;status=xr_xir_compile_checked_write(source.checked,&first,NULL);
    xr_xir_compile_source_result_free(&source);
    if(status!=XR_XIR_OK) {CHECK(!first.bytes && !first.length);goto done;}
    ++receiver_compile_calls;status=xr_xir_compile_checked_read(context,first.bytes,first.length,&read,NULL);
    memset(first.bytes,0xa5,first.length);xr_xir_compile_checked_packet_free(&first);
    if(status!=XR_XIR_OK) {CHECK(!read);goto done;}
    ++receiver_compile_calls;status=xr_xir_compile_specialize(read,&closed,NULL);
    xr_xir_compile_artifact_free(read);read=NULL;
    if(status!=XR_XIR_OK) {CHECK(!closed);goto done;}
    ++receiver_compile_calls;status=xr_xir_compile_checked_write(closed,&second,NULL);
    xr_xir_compile_artifact_free(closed);closed=NULL;
    if(status!=XR_XIR_OK) {CHECK(!second.bytes && !second.length);goto done;}
    ++receiver_compile_calls;status=xr_xir_compile_checked_read(context,second.bytes,second.length,&reloaded,NULL);
    memset(second.bytes,0xa5,second.length);xr_xir_compile_checked_packet_free(&second);
    if(status!=XR_XIR_OK) {CHECK(!reloaded);goto done;}
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    ++receiver_compile_calls;status=xr_xir_compile_lower(reloaded,&target, NULL,&lowered,NULL);
    xr_xir_compile_artifact_free(reloaded);reloaded=NULL;
    if(status!=XR_XIR_OK) {CHECK(!lowered);goto done;}
    ++receiver_compile_calls;status=xr_xir_compile_artifact_verify(lowered,NULL);
    if(status!=XR_XIR_OK) goto done;
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=receiver_compile_entry(module);
    CHECK(entry!=UINT32_MAX);
    ++receiver_compile_calls;status=xr_xir_compile_emit_c(lowered,"receiver_compiler",1048576,&emitted);
    if(status!=XR_XIR_OK) {CHECK(!emitted.text && !emitted.length);goto done;}
    CHECK(emitted.text && emitted.length && !strstr(emitted.text,"({"));xr_xir_compile_c_source_free(&emitted);
    ++receiver_compile_calls;status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status==XR_XIR_OK) {CHECK(program && !lowered);if(result->keep){result->program=program;result->entry=entry;program=NULL;}}
    else CHECK(!program && lowered);
done:
    xr_xir_compile_program_drop(program);xr_xir_compile_c_source_free(&emitted);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(reloaded);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(read);xr_xir_compile_checked_packet_free(&second);xr_xir_compile_checked_packet_free(&first);
    xr_xir_compile_source_result_free(&source);xr_compile_resources_free(failure);
    return status;
}
static LibraryCompileOperation receiver_fault_operation=receiver_compile_operation;

/* The real resource allocator creates two requests only after its second OOM.
 * This makes a flat primary sweep observably incomplete. */
static XrXirStatus receiver_allocator_audit_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque;void *a=NULL,*b=NULL,*c=NULL,*d=NULL;
    XrCompileResourceStatus first=xr_compile_resources_alloc(context->resources,8,&a);
    if(first==XR_COMPILE_RESOURCE_OK) {xr_compile_resources_free(a);return XR_XIR_OK;}
    CHECK(first==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    XrCompileResourceStatus second=xr_compile_resources_alloc(context->resources,16,&b);
    CHECK(second==XR_COMPILE_RESOURCE_OK || second==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    if(second==XR_COMPILE_RESOURCE_OUT_OF_MEMORY) {
        XrCompileResourceStatus third=xr_compile_resources_alloc(context->resources,24,&c);
        CHECK(third==XR_COMPILE_RESOURCE_OK || third==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        if(third==XR_COMPILE_RESOURCE_OK) {
            XrCompileResourceStatus fourth=xr_compile_resources_alloc(context->resources,32,&d);
            CHECK(fourth==XR_COMPILE_RESOURCE_OK || fourth==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        }
    }
    xr_compile_resources_free(d);xr_compile_resources_free(c);xr_compile_resources_free(b);return XR_XIR_OUT_OF_MEMORY;
}

static uint64_t receiver_compile_number(const char *text) {
    CHECK(text && *text);for(const char *p=text;*p;++p)CHECK(*p>='0' && *p<='9');
    errno=0;char *end=NULL;unsigned long long value=strtoull(text,&end,10);CHECK(!errno && end && !*end);return value;
}
static void receiver_compile_hex(const char *text,size_t length) {
    CHECK(text && strlen(text)==length);
    for(size_t i=0;i<length;++i)CHECK((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f'));
}
static void receiver_compile_stats(FILE *report,const XrCompileResourceStats *stats) {
    CHECK(fprintf(report,"\"allocated\":%llu,\"live\":%llu,\"peak\":%llu,\"work\":%llu,\"allocations\":%llu",
        (unsigned long long)stats->allocated_bytes,(unsigned long long)stats->live_bytes,
        (unsigned long long)stats->peak_bytes,(unsigned long long)stats->work,(unsigned long long)stats->allocation_count)>0);
}
static void receiver_compile_preamble(FILE *report,const char *mode,uint64_t index,uint64_t sites,const char *nonce) {
    const char *input=getenv("XRAY_GENERIC_SHARD_INPUT_SHA256"),*binary=getenv("XRAY_GENERIC_SHARD_BINARY_SHA256");
    const char *config=getenv("XRAY_GENERIC_SHARD_CONFIG_SHA256");
    receiver_compile_hex(nonce,32);receiver_compile_hex(input,64);receiver_compile_hex(binary,64);receiver_compile_hex(config,64);
    uint8_t digest[32];xr_sha256((const uint8_t *)receiver_compile_source,sizeof(receiver_compile_source)-1,digest);
    CHECK(fprintf(report,"{\"event\":\"identity\",\"schema\":\"%s\",\"mode\":\"%s\","
        "\"index\":%llu,\"sites\":%llu,\"workers\":8,\"nonce\":\"%s\",\"input_sha256\":\"%s\","
        "\"binary_sha256\":\"%s\",\"config_sha256\":\"%s\",\"source_bytes\":%zu,\"source_sha256\":\"",
        receiver_compile_schema,mode,(unsigned long long)index,(unsigned long long)sites,nonce,input,binary,config,sizeof(receiver_compile_source)-1)>0);
    for(size_t i=0;i<sizeof(digest);++i)CHECK(fprintf(report,"%02x",(unsigned)digest[i])==2);
    CHECK(fprintf(report,"\",\"allocated_cap\":67108864,\"live_cap\":8388608,\"work_cap\":128000000}\n")>0);
}
static void receiver_compile_complete(FILE *report,size_t points,size_t histories) {
    library_compile_observer_free();CHECK(!source_program_compile_live && !source_program_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"histories\":%zu,\"compiler_blocks\":0,\"compiler_bytes\":0}\n",points,histories)>0);
    CHECK(!ferror(report) && !fflush(report) && !fclose(report));
}
static void receiver_compile_census(FILE *report) {
    XrCompileResources *unpublished=NULL;
    source_program_compile_attempts=0;source_program_compile_fail_at=0;source_program_compile_injected=false;
    CHECK(xr_compile_resources_new(&library_compile_limits,&unpublished)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    source_program_compile_fail_at=SIZE_MAX;CHECK(!unpublished && source_program_compile_injected && source_program_compile_attempts==1);
    LibraryCompileOwner owner={0};ReceiverCompileResult result={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    receiver_compile_calls=0;source_program_compile_attempts=0;
    CHECK(receiver_compile_operation(&owner.context,&result)==XR_XIR_OK && receiver_compile_calls==10);
    size_t sites=source_program_compile_attempts;CHECK(sites && sites<=65000);
    XrCompileResourceStats stats=library_compile_stats(&owner.context);uint64_t baseline=owner.baseline.live_bytes;
    CHECK(stats.live_bytes==baseline);library_compile_owner_drop(&owner);
    CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"public_calls\":10,\"ledger_OOM\":1,\"owner_live\":%llu,",
        sites,(unsigned long long)baseline)>0);receiver_compile_stats(report,&stats);
    CHECK(fprintf(report,",\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n",source_program_compile_live,source_program_compile_bytes)>0);
    receiver_compile_complete(report,0,0);
}
static size_t receiver_compile_fault(FILE *report,const size_t *faults,size_t count) {
    CHECK(count && count<=65000 && faults);
    for(size_t i=1;i<count;++i)CHECK(faults[i]>faults[i-1]);
    size_t ordinal=faults[0];
    LibraryCompileOwner owner={0};ReceiverCompileResult result={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    receiver_failure_plan=faults;receiver_failure_count=count;receiver_failure_next=1;
    receiver_compile_calls=0;source_program_compile_attempts=0;source_program_compile_fail_at=ordinal;source_program_compile_injected=false;
    XrXirStatus status=receiver_fault_operation(&owner.context,&result);source_program_compile_fail_at=SIZE_MAX;
    receiver_failure_plan=NULL;receiver_failure_count=0;
    if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Receiver compiler ordinal%zu depth%zu status%u calls%u\n",ordinal,count,status,receiver_compile_calls);
    CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected && receiver_failure_next==count &&
        source_program_compile_attempts>faults[count-1] && source_program_compile_attempts<=65000);
    size_t attempts=source_program_compile_attempts;XrCompileResourceStats stats=library_compile_stats(&owner.context);
    uint64_t baseline=owner.baseline.live_bytes;CHECK(stats.live_bytes==baseline);library_compile_owner_drop(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(fprintf(report,"{\"event\":\"point\",\"ordinal\":%zu,\"history\":[",ordinal)>0);
    for(size_t i=0;i<count;++i)CHECK(fprintf(report,"%s%zu",i?",":"",faults[i])>0);
    CHECK(fprintf(report,"],\"fired\":%zu,\"status\":%u,\"injected\":true,\"attempts\":%zu,\"owner_live\":%llu,",
        count,status,attempts,(unsigned long long)baseline)>0);receiver_compile_stats(report,&stats);
    CHECK(fprintf(report,",\"compiler_blocks\":0,\"compiler_bytes\":0}\n")>0);return attempts;
}
typedef struct ReceiverFaultLevel {size_t next,end;} ReceiverFaultLevel;
/* Iterative depth-first enumeration covers every allocation attempted after
 * every injected prefix. Each complete history starts with a fresh ledger. */
static size_t receiver_compile_fault_tree(FILE *report,size_t ordinal) {
    size_t *faults=receiver_history_storage(65000*sizeof(*faults));
    ReceiverFaultLevel *levels=receiver_history_storage(65000*sizeof(*levels));
    CHECK(faults && levels);size_t count=1,histories=1;faults[0]=ordinal;
    levels[0]=(ReceiverFaultLevel){ordinal+1,receiver_compile_fault(report,faults,count)};
    while(count) {
        ReceiverFaultLevel *parent=&levels[count-1];
        if(parent->next==parent->end) {--count;continue;}
        CHECK(parent->next<parent->end && count<65000 && histories<SIZE_MAX);
        size_t next=parent->next++;faults[count++]=next;++histories;
        levels[count-1]=(ReceiverFaultLevel){next+1,receiver_compile_fault(report,faults,count)};
    }
    receiver_history_storage_free(levels);receiver_history_storage_free(faults);return histories;
}
static void receiver_compile_partition(FILE *report,size_t index,size_t sites) {
    size_t points=0,histories=0;
    for(size_t ordinal=index;ordinal<sites;ordinal+=8) {
        size_t count=receiver_compile_fault_tree(report,ordinal);CHECK(count<=SIZE_MAX-histories);histories+=count;++points;
    }
    receiver_compile_complete(report,points,histories);
}
static void receiver_compile_bounds(FILE *report,const uint64_t required[3]) {
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(required[axis] && required[axis]<=*cap);*cap=required[axis]-less;
        LibraryCompileOwner owner={0};ReceiverCompileResult result={0};source_program_compile_attempts=0;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if(status==XR_XIR_OK)status=receiver_compile_operation(&owner.context,&result);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        XrCompileResourceStats stats={0};uint64_t baseline=0;
        if(owner.context.resources){stats=library_compile_stats(&owner.context);baseline=owner.baseline.live_bytes;CHECK(stats.live_bytes==baseline);}
        library_compile_owner_drop(&owner);CHECK(!runtime_live && !runtime_bytes);
        CHECK(fprintf(report,"{\"event\":\"axis\",\"axis\":%u,\"less\":%u,\"exact\":%llu,\"status\":%u,\"owner_live\":%llu,"
            "\"allocated_cap\":%llu,\"live_cap\":%llu,\"work_cap\":%llu,",axis,less,(unsigned long long)required[axis],status,
            (unsigned long long)baseline,(unsigned long long)limits.allocated_bytes,(unsigned long long)limits.live_bytes,(unsigned long long)limits.work)>0);
        receiver_compile_stats(report,&stats);CHECK(fprintf(report,",\"compiler_blocks\":0,\"compiler_bytes\":0}\n")>0);
    }
    receiver_compile_complete(report,6,0);
}
int main(int argc,char **argv) {
    const char *stdlib=getenv("XRAY_RECEIVER_FI_STDLIB");if(stdlib) {CHECK(*stdlib);receiver_compile_stdlib=stdlib;}
    if(argc==3 && !strcmp(argv[1],"--history-audit")) {
        LibraryCompileOwner owner={0};ReceiverCompileResult result={0};
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);source_program_compile_attempts=0;
        CHECK(receiver_allocator_audit_operation(&owner.context,&result)==XR_XIR_OK && source_program_compile_attempts==1);
        library_compile_owner_drop(&owner);FILE *report=fopen(argv[2],"wb");CHECK(report);
        CHECK(fprintf(report,"{\"event\":\"audit\",\"schema\":\"receiver-allocator-history.v1\",\"normal_sites\":1}\n")>0);
        receiver_fault_operation=receiver_allocator_audit_operation;
        CHECK(receiver_compile_fault_tree(report,0)==4);receiver_fault_operation=receiver_compile_operation;
        receiver_compile_complete(report,1,4);return 0;
    }
    if(argc==1) {
        LibraryCompileOwner owner={0};ReceiverCompileResult result={.keep=true};
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
        source_program_compile_attempts=0;receiver_compile_calls=0;
        CHECK(receiver_compile_operation(&owner.context,&result)==XR_XIR_OK && result.program && receiver_compile_calls==10);
        size_t sites=source_program_compile_attempts;CHECK(sites && sites<=65000);
        receiver_compile_oracle(result.program,result.entry);result.program=NULL;
        XrCompileResourceStats stats=library_compile_stats(&owner.context);library_compile_owner_drop(&owner);library_compile_observer_free();
        CHECK(!source_program_compile_live && !source_program_compile_bytes && !runtime_live && !runtime_bytes);
        printf("Generator ten-stage Source/codec/specialize/lower/verify/Cgen/VM: fixture oracle sites%zu work%llu allocated%llu peak%llu physical0 PASS\n",
            sites,(unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes);return 0;
    }
    bool census=!strcmp(argv[1],"--compiler-census"),shard=!strcmp(argv[1],"--compiler-shard"),bounds=!strcmp(argv[1],"--compiler-bounds");
    bool probe=!strcmp(argv[1],"--compiler-probe");
    CHECK(census || shard || bounds || probe);CHECK(argc==(census?4:(shard || probe)?6:8));
    uint64_t index=0,sites=0,required[3]={0};
    if(!census){sites=receiver_compile_number(argv[(shard || probe)?3:2]);CHECK(sites && sites<=65000);}
    if(shard){index=receiver_compile_number(argv[2]);CHECK(index<8);}
    if(probe){index=receiver_compile_number(argv[2]);CHECK(index<sites);}
    if(bounds)for(unsigned axis=0;axis<3;++axis)required[axis]=receiver_compile_number(argv[3+axis]);
    const char *nonce=argv[argc-2];receiver_compile_hex(nonce,32);FILE *report=fopen(argv[argc-1],"wb");CHECK(report);
    receiver_compile_preamble(report,census?"census":shard?"shard":probe?"probe":"bounds",index,sites,nonce);
    if(census)receiver_compile_census(report);else if(shard)receiver_compile_partition(report,(size_t)index,(size_t)sites);
    else if(probe){size_t ordinal=(size_t)index;receiver_compile_fault(report,&ordinal,1);receiver_compile_complete(report,1,1);}
    else receiver_compile_bounds(report,required);return 0;
}

#endif // XIR_GENERATOR_COMPILER_FAULT_CASES_H
