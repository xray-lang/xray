/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_copy_bytes_resources.h - Whole cold UTF-8 byte-copy compiler requests
 *
 * KEY CONCEPT:
 *   Source, CTFE, both constructions, receiving and Program sealing spend one ledger.
 */
#ifndef XIR_STRING_COPY_BYTES_RESOURCES_H
#define XIR_STRING_COPY_BYTES_RESOURCES_H
#include "base/xsha256.h"
#include <errno.h>
typedef struct ByteCopyResourceReport {
 XrXirStatus status;XrCompileResourceStats spent;size_t sites;bool owner;
} ByteCopyResourceReport;
static const XrCompileResourceLimits byte_resource_caps={67108864,8388608,128000000};
static XrCompileResourceStats byte_resource_stats(const XrXirCompileContext *context) {
 XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void byte_resource_monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
 CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>=before.work && after.allocation_count>=before.allocation_count && after.peak_bytes>=before.peak_bytes);
}
static XrXirStatus byte_resource_whole(const XrXirCompileContext *context,const XrXirSourceText *input) {
 XrCompilerSession *session=NULL;XrXirSourceResult source={0};XrXirCheckedPacket packet={0};
 XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;
 XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
 if(opened!=XR_COMPILER_SESSION_OK){CHECK(!session);return opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;}
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"string-copy-bytes-resource-source",NULL};
 XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,input,&source,&diagnostic,NULL);
 if(status!=XR_XIR_OK){CHECK(!source.checked && !source.snapshot);goto done;}
 const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);CHECK(source.checked && view && view->complete && view->expression_count);
 status=xr_xir_compile_checked_write(source.checked,&packet,NULL);xr_xir_compile_source_result_free(&source);
 if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
 if(packet.bytes)memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 if(status==XR_XIR_OK)status=xr_xir_compile_specialize(received,&closed,NULL);xr_xir_compile_artifact_free(received);received=NULL;
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);xr_xir_compile_artifact_free(closed);closed=NULL;
 if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,&program);
done:
 xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(received);
 xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);return status;
}
static void byte_resource_trial(const XrXirSourceText *input,
    const XrCompileResourceLimits *caps,size_t fault,bool retry,ByteCopyResourceReport *output) {
    size_t held_blocks=source_fixture_compile_live,held_bytes=source_fixture_compile_bytes;
    size_t start=source_fixture_compile_attempts;XrXirCompileContext context={0};ByteCopyResourceReport report={0};
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    XrCompileResourceStatus opened=xr_compile_resources_new(caps,&context.resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources);
        report.status=XR_XIR_OUT_OF_MEMORY;goto done;
    }
    report.owner=true;context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline=byte_resource_stats(&context);
    report.status=byte_resource_whole(&context,input);report.sites=source_fixture_compile_attempts-start;
    report.spent=byte_resource_stats(&context);byte_resource_monotonic(baseline,report.spent);
    CHECK(report.spent.live_bytes==baseline.live_bytes && report.spent.allocated_bytes<=caps->allocated_bytes &&
        report.spent.peak_bytes<=caps->live_bytes && report.spent.work<=caps->work);
    source_fixture_compile_fail_at=SIZE_MAX;
    if (retry) {
        CHECK(report.status==XR_XIR_BUDGET || report.status==XR_XIR_OUT_OF_MEMORY);
        XrCompileResources *same=context.resources;
        XrXirStatus again=byte_resource_whole(&context,input);
        CHECK(again==(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET));
        XrCompileResourceStats after=byte_resource_stats(&context);byte_resource_monotonic(report.spent,after);
        CHECK(context.resources==same && after.live_bytes==baseline.live_bytes &&
            after.allocated_bytes<=caps->allocated_bytes && after.peak_bytes<=caps->live_bytes && after.work<=caps->work);
    }
    xr_compile_resources_release(context.resources);
done:
    if (!report.sites) report.sites=source_fixture_compile_attempts-start;
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);*output=report;
}
static void byte_compiler_resources(const XrXirSourceText *input,bool full) {
    ByteCopyResourceReport first={0},second={0};
    byte_resource_trial(input,&byte_resource_caps,SIZE_MAX,false,&first);
    byte_resource_trial(input,&byte_resource_caps,SIZE_MAX,false,&second);
    if (first.status!=XR_XIR_OK || second.status!=XR_XIR_OK || first.sites!=second.sites ||
        memcmp(&first.spent,&second.spent,sizeof(first.spent)))
        fprintf(stderr,"Owned UTF-8 copy census got%u/%u sites%zu/%zu\n",
            (unsigned)first.status,(unsigned)second.status,first.sites,second.sites);
    CHECK(first.status==XR_XIR_OK && first.owner && first.sites>2 && second.status==XR_XIR_OK &&
        second.sites==first.sites && !memcmp(&first.spent,&second.spent,sizeof(first.spent)));
    fprintf(stderr,"Owned UTF-8 copy compiler census=%zu allocated=%llu peak=%llu work=%llu\n",first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.peak_bytes,
        (unsigned long long)first.spent.work);fflush(stderr);
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits caps=byte_resource_caps;
        uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;
        CHECK(exact>1);
        if (!axis) caps.allocated_bytes=exact-less;
        else if (axis==1) caps.live_bytes=exact-less;
        else caps.work=exact-less;
        ByteCopyResourceReport report={0};byte_resource_trial(input,&caps,SIZE_MAX,less!=0,&report);
        if (report.status!=(less?XR_XIR_BUDGET:XR_XIR_OK))
            fprintf(stderr,"Owned UTF-8 copy axis%u less%u got%u\n",axis,less,(unsigned)report.status);
        CHECK(report.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if (!less) CHECK(report.sites==first.sites && !memcmp(&report.spent,&first.spent,sizeof(first.spent)));
    }
    size_t samples[]={0,first.sites/2,first.sites-1};
    for (size_t i=0;i<(full?first.sites:3);++i) {
        size_t fault=full?i:samples[i];ByteCopyResourceReport report={0};
        byte_resource_trial(input,&byte_resource_caps,fault,true,&report);
        if (report.status!=XR_XIR_OUT_OF_MEMORY || report.sites!=fault+1)
            fprintf(stderr,"Owned UTF-8 copy compiler FI%zu got%u sites%zu\n",fault,(unsigned)report.status,report.sites);
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && report.sites==fault+1);
    }
    fprintf(stderr,"Owned UTF-8 copy compiler: %s FI=%zu, original-owner retry, six axes, physical baseline restored\n",
        full?"complete":"three sampled",full?first.sites:(size_t)3);
}
static size_t byte_fault_number(const char *text) {
 CHECK(text && *text);for(const char *p=text;*p;++p)CHECK(*p>='0' && *p<='9');
 errno=0;char *end=NULL;unsigned long long value=strtoull(text,&end,10);CHECK(!errno && end && !*end && value<=SIZE_MAX);return (size_t)value;
}
static void byte_compiler_partition(char **argv) {
 size_t first=byte_fault_number(argv[3]),end=byte_fault_number(argv[4]),sites=byte_fault_number(argv[5]);CHECK(first<=end && end<=sites && (sites || (!first && !end)));
 FILE *file=fopen(argv[2],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length==(long)sizeof(byte_copy_source)-1 && !fseek(file,0,SEEK_SET));
 char *bytes=xr_malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file) && !memcmp(bytes,byte_copy_source,(size_t)length));
 XrXirSourceText input={NULL,bytes,(size_t)length};FILE *report=fopen(argv[6],"wb");CHECK(report);uint8_t digest[32];xr_sha256((const uint8_t *)bytes,(size_t)length,digest);
 CHECK(fprintf(report,"{\"event\":\"input\",\"first\":%zu,\"end\":%zu,\"sites\":%zu,\"source_sha256\":\"",first,end,sites)>0);
 for(size_t i=0;i<sizeof(digest);++i)CHECK(fprintf(report,"%02x",(unsigned)digest[i])==2);CHECK(fprintf(report,"\",\"caps\":[67108864,8388608,128000000]}\n")>0);
 if(!sites){ByteCopyResourceReport census={0};byte_resource_trial(&input,&byte_resource_caps,SIZE_MAX,false,&census);CHECK(census.status==XR_XIR_OK && census.owner && census.sites>2);
  CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu}\n",census.sites,(unsigned long long)census.spent.allocated_bytes,(unsigned long long)census.spent.peak_bytes,(unsigned long long)census.spent.work)>0);
 }else for(size_t fault=first;fault<end;++fault){ByteCopyResourceReport trial={0};byte_resource_trial(&input,&byte_resource_caps,fault,true,&trial);
  if(trial.status!=XR_XIR_OUT_OF_MEMORY || !source_fixture_compile_injected || trial.sites!=fault+1)fprintf(stderr,"Byte-copy compiler fault%zu status%u attempts%zu\n",fault,(unsigned)trial.status,trial.sites);
  CHECK(trial.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && trial.sites==fault+1);
  CHECK(fprintf(report,"{\"event\":\"fault\",\"ordinal\":%zu,\"attempts\":%zu,\"owner\":%u}\n",fault,trial.sites,(unsigned)trial.owner)>0);
 }
 CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"compiler_blocks\":0,\"compiler_bytes\":0}\n",end-first)>0 && !fclose(report));xr_free(bytes);CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_STRING_COPY_BYTES_RESOURCES_H
