/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_json_resources.h - Actual automatic derivation refusals
 *
 * KEY CONCEPT:
 *   Every trial receives the authentic library packet on one finite ledger.
 *   Source includes generator execution, both constructions and packet death.
 */
#ifndef XIR_SOURCE_DERIVATION_JSON_RESOURCES_H
#define XIR_SOURCE_DERIVATION_JSON_RESOURCES_H
#include <errno.h>
#include "xir/xxir_derivation_internal.h"
typedef struct JsonResourceReport {
    XrXirStatus status;
    XrCompileResourceStats spent;
    size_t sites;
    bool owner;
} JsonResourceReport;
static const XrCompileResourceLimits json_resource_caps={67108864,8388608,128000000};
static XrCompileResourceStats json_resource_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void json_resource_monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
    CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>=before.work &&
        after.allocation_count>=before.allocation_count && after.peak_bytes>=before.peak_bytes);
}
static XrXirStatus json_resource_whole(const XrXirCompileContext *context,
    const XrXirCheckedPacket *input,const char *path) {
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_JSON_WORK},"generator_json.xr"};
    XrXirLibraryInput packet={input->bytes,input->length,{0},&binding,1};
    xr_sha256(input->bytes,input->length,packet.sha256);
    XrXirLibraryCatalog *catalog=NULL;XrCompilerSession *session=NULL;
    XrXirSourceResult source={0};XrXirArtifact *received=NULL;XrXirCheckedPacket written={0};
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&packet,1,&catalog);
    if (status!=XR_XIR_OK) {CHECK(!catalog);goto done;}
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);status=opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;goto done;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_DERIVATION_JSON_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    XrXirSourceText text={"future_json.xr",json_user_source,sizeof(json_user_source)-1};XrXirSourceDiagnostic diagnostic={0};
    status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) {CHECK(!source.checked && !source.snapshot);goto done;}
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source.snapshot);
    CHECK(source.checked && view && view->complete && view->declaration_count && view->expression_count);
    status=xr_xir_compile_checked_write(source.checked,&written,NULL);
    xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,written.bytes,written.length,&received,NULL);
    if (written.bytes) memset(written.bytes,0xa5,written.length);
    xr_xir_compile_checked_packet_free(&written);
    if (status==XR_XIR_OK) {
        CHECK(received);const XrXirModule *module=xr_xir_compile_artifact_module(received);
        CHECK(module->declarations->derivations && module->declarations->derivations->count==1);
        status=xr_xir_compile_artifact_verify(received,NULL);
    }
done:
    xr_xir_compile_checked_packet_free(&written);xr_xir_compile_artifact_free(received);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    xr_xir_compile_library_catalog_free(catalog);return status;
}
static void json_resource_trial(const XrXirCheckedPacket *packet,const char *path,
    const XrCompileResourceLimits *caps,size_t fault,bool retry,JsonResourceReport *output) {
    size_t held_blocks=source_fixture_compile_live,held_bytes=source_fixture_compile_bytes;
    size_t start=source_fixture_compile_attempts;XrXirCompileContext context={0};JsonResourceReport report={0};
    source_fixture_compile_injected=false;source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    XrCompileResourceStatus opened=xr_compile_resources_new(caps,&context.resources);
    if (opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources);
        report.status=XR_XIR_OUT_OF_MEMORY;goto done;
    }
    report.owner=true;context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline=json_resource_stats(&context);
    report.status=json_resource_whole(&context,packet,path);report.sites=source_fixture_compile_attempts-start;
    report.spent=json_resource_stats(&context);json_resource_monotonic(baseline,report.spent);
    CHECK(report.spent.live_bytes==baseline.live_bytes && report.spent.allocated_bytes<=caps->allocated_bytes &&
        report.spent.peak_bytes<=caps->live_bytes && report.spent.work<=caps->work);
    source_fixture_compile_fail_at=SIZE_MAX;
    if (retry) {
        CHECK(report.status==XR_XIR_BUDGET || report.status==XR_XIR_OUT_OF_MEMORY);
        XrCompileResources *same=context.resources;
        XrXirStatus again=json_resource_whole(&context,packet,path);
        CHECK(again==(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET));
        XrCompileResourceStats after=json_resource_stats(&context);json_resource_monotonic(report.spent,after);
        CHECK(context.resources==same && after.live_bytes==baseline.live_bytes &&
            after.allocated_bytes<=caps->allocated_bytes && after.peak_bytes<=caps->live_bytes && after.work<=caps->work);
    }
    xr_compile_resources_release(context.resources);
done:
    if (!report.sites) report.sites=source_fixture_compile_attempts-start;
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_live==held_blocks && source_fixture_compile_bytes==held_bytes);*output=report;
}
static void json_compiler_resources(const XrXirCheckedPacket *packet,const char *path,bool full) {
    JsonResourceReport first={0},second={0};
    json_resource_trial(packet,path,&json_resource_caps,SIZE_MAX,false,&first);
    json_resource_trial(packet,path,&json_resource_caps,SIZE_MAX,false,&second);
    if (first.status!=XR_XIR_OK || second.status!=XR_XIR_OK || first.sites!=second.sites ||
        memcmp(&first.spent,&second.spent,sizeof(first.spent)))
        fprintf(stderr,"Ordinary JSON derive census got%u/%u sites%zu/%zu\n",
            (unsigned)first.status,(unsigned)second.status,first.sites,second.sites);
    CHECK(first.status==XR_XIR_OK && first.owner && first.sites>2 && second.status==XR_XIR_OK &&
        second.sites==first.sites && !memcmp(&first.spent,&second.spent,sizeof(first.spent)));
    fprintf(stderr,"Ordinary JSON derive compiler census=%zu allocated=%llu peak=%llu work=%llu\n",first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.peak_bytes,
        (unsigned long long)first.spent.work);fflush(stderr);
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits caps=json_resource_caps;
        uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;
        CHECK(exact>1);
        if (!axis) caps.allocated_bytes=exact-less;
        else if (axis==1) caps.live_bytes=exact-less;
        else caps.work=exact-less;
        JsonResourceReport report={0};json_resource_trial(packet,path,&caps,SIZE_MAX,less!=0,&report);
        if (report.status!=(less?XR_XIR_BUDGET:XR_XIR_OK))
            fprintf(stderr,"Ordinary JSON derive axis%u less%u got%u\n",axis,less,(unsigned)report.status);
        CHECK(report.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if (!less) CHECK(report.sites==first.sites && !memcmp(&report.spent,&first.spent,sizeof(first.spent)));
    }
    size_t samples[]={0,first.sites/2,first.sites-1};
    for (size_t i=0;i<(full?first.sites:3);++i) {
        size_t fault=full?i:samples[i];JsonResourceReport report={0};
        json_resource_trial(packet,path,&json_resource_caps,fault,true,&report);
        if (report.status!=XR_XIR_OUT_OF_MEMORY || report.sites!=fault+1)
            fprintf(stderr,"Ordinary JSON derive compiler FI%zu got%u sites%zu\n",fault,(unsigned)report.status,report.sites);
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && report.sites==fault+1);
    }
    fprintf(stderr,"Ordinary JSON derive compiler: %s FI=%zu, original-owner retry, six axes, physical baseline restored\n",
        full?"complete":"three sampled",full?first.sites:(size_t)3);
}
static size_t json_fault_number(const char *text) {
    CHECK(text && *text);
    for (const char *p=text;*p;++p) CHECK(*p>='0' && *p<='9');
    errno=0;char *end=NULL;unsigned long long value=strtoull(text,&end,10);
    CHECK(!errno && end && !*end && value<=SIZE_MAX);return (size_t)value;
}
static void json_compiler_partition(char **argv) {
    size_t first=json_fault_number(argv[3]),end=json_fault_number(argv[4]),sites=json_fault_number(argv[5]);
    CHECK(first<=end && end<=sites && (sites || (!first && !end)));
    FILE *input=fopen(argv[2],"rb");CHECK(input && !fseek(input,0,SEEK_END));
    long length=ftell(input);CHECK(length>0 && length<=67108864 && !fseek(input,0,SEEK_SET));
    uint8_t *bytes=xr_malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,input)==(size_t)length && !fclose(input));
    XrXirCheckedPacket packet={0};packet.bytes=bytes;packet.length=(size_t)length;
    char path[2048];int count=snprintf(path,sizeof(path),"%s/future_json.xr",XR_SOURCE_DERIVATION_JSON_WORK);
    CHECK(count>0 && count<(int)sizeof(path));FILE *report=fopen(argv[6],"wb");CHECK(report);
    uint8_t digest[32];xr_sha256(bytes,(size_t)length,digest);
    CHECK(fprintf(report,"{\"event\":\"input\",\"first\":%zu,\"end\":%zu,\"sites\":%zu,\"packet_sha256\":\"",first,end,sites)>0);
    for (size_t i=0;i<sizeof(digest);++i) CHECK(fprintf(report,"%02x",(unsigned)digest[i])==2);
    CHECK(fprintf(report,"\",\"caps\":[67108864,8388608,128000000]}\n")>0);
    if (!sites) {
        JsonResourceReport census={0};json_resource_trial(&packet,path,&json_resource_caps,SIZE_MAX,false,&census);
        CHECK(census.status==XR_XIR_OK && census.owner && census.sites>2);
        CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu}\n",
            census.sites,(unsigned long long)census.spent.allocated_bytes,
            (unsigned long long)census.spent.peak_bytes,(unsigned long long)census.spent.work)>0);
    } else for (size_t fault=first;fault<end;++fault) {
        JsonResourceReport trial={0};json_resource_trial(&packet,path,&json_resource_caps,fault,true,&trial);
        CHECK(trial.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && trial.sites==fault+1);
        CHECK(fprintf(report,"{\"event\":\"fault\",\"ordinal\":%zu,\"attempts\":%zu,\"owner\":%u}\n",
            fault,trial.sites,(unsigned)trial.owner)>0);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"compiler_blocks\":0,\"compiler_bytes\":0}\n",end-first)>0 && !fclose(report));
    xr_free(bytes);
}
#endif // XIR_SOURCE_DERIVATION_JSON_RESOURCES_H
