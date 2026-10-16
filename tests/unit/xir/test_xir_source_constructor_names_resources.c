/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_constructor_names_resources.c - Finite actual name receiving
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_generated_schema.h"
#include "xir_source_constructor_names_source.h"
typedef struct NamesReport {XrXirStatus status;XrCompileResourceStats spent;size_t sites;bool owner;} NamesReport;
static const XrCompileResourceLimits names_caps={67108864,8388608,128000000};
#ifndef XR_CTOR_NAMES_RESOURCE_VARIANT
#define XR_CTOR_NAMES_RESOURCE_VARIANT 0
#endif
static const struct {const char *left,*right,*expected;bool construct;} names_cases[]={
    {"left","right","left:right",true},{"right","left","right:left",true},
    {"renamedLeft","renamedRight","renamedLeft:renamedRight",true},{"hiddenLeft","hiddenRight","none",false}};
static void names_case_source(unsigned variant,char *source,size_t capacity) {
    CHECK(variant<4);int n=snprintf(source,capacity,names_user,"generate",names_cases[variant].construct?",construct:true":"",
        names_cases[variant].left,names_cases[variant].right,names_cases[variant].left,names_cases[variant].right,names_cases[variant].expected);
    CHECK(n>0 && (size_t)n<capacity);
}
static XrCompileResourceStats names_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void names_monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
    CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>=before.work &&
        after.allocation_count>=before.allocation_count && after.peak_bytes>=before.peak_bytes);
}
static XrXirStatus names_whole(const XrXirCompileContext *context,const XrXirCheckedPacket *input,const char *bytes) {
    XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CTOR_NAMES_WORK},"constructor_names.xr"};
    XrXirLibraryInput packet={input->bytes,input->length,{0},&binding,1};xr_sha256(input->bytes,input->length,packet.sha256);
    XrXirLibraryCatalog *catalog=NULL;XrCompilerSession *session=NULL;XrXirSourceResult source={0};
    XrXirCheckedPacket written={0};XrXirArtifact *received=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&packet,1,&catalog);
    if(status!=XR_XIR_OK){CHECK(!catalog);goto done;}
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);status=opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;goto done;
    }
    char path[2048];int n=snprintf(path,sizeof(path),"%s/future_constructor_names.xr",XR_CTOR_NAMES_WORK);CHECK(n>0 && n<(int)sizeof(path));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CTOR_NAMES_WORK};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,XR_XIR_PROGRAM,catalog,NULL};
    XrXirSourceText text={"future_constructor_names.xr",bytes,strlen(bytes)};XrXirSourceDiagnostic diagnostic={0};
    status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
    if(status!=XR_XIR_OK){CHECK(!source.checked && !source.snapshot);goto done;}
    CHECK(source.checked && source.snapshot);status=xr_xir_compile_checked_write(source.checked,&written,NULL);
    xr_xir_compile_source_result_free(&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,written.bytes,written.length,&received,NULL);
    if(written.bytes)memset(written.bytes,0xa5,written.length);xr_xir_compile_checked_packet_free(&written);
    if(status==XR_XIR_OK) {CHECK(received);status=xr_xir_compile_artifact_verify(received,NULL);}
done:
    xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&written);
    xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return status;
}
static void names_trial(const XrXirCheckedPacket *packet,const char *source,const XrCompileResourceLimits *caps,
    size_t fault,bool retry,NamesReport *output) {
    size_t held=source_fixture_compile_live,held_bytes=source_fixture_compile_bytes,start=source_fixture_compile_attempts;
    XrXirCompileContext context={0};NamesReport report={0};source_fixture_compile_injected=false;
    source_fixture_compile_fail_at=fault==SIZE_MAX?SIZE_MAX:start+fault;
    XrCompileResourceStatus opened=xr_compile_resources_new(caps,&context.resources);
    if(opened!=XR_COMPILE_RESOURCE_OK) {
        CHECK(opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !context.resources);report.status=XR_XIR_OUT_OF_MEMORY;goto done;
    }
    context.limits=xr_xir_compile_default_limits();report.owner=true;XrCompileResourceStats baseline=names_stats(&context);
    report.status=names_whole(&context,packet,source);report.sites=source_fixture_compile_attempts-start;report.spent=names_stats(&context);
    names_monotonic(baseline,report.spent);CHECK(report.spent.live_bytes==baseline.live_bytes && report.spent.allocated_bytes<=caps->allocated_bytes &&
        report.spent.peak_bytes<=caps->live_bytes && report.spent.work<=caps->work);source_fixture_compile_fail_at=SIZE_MAX;
    if(retry) {
        XrCompileResources *same=context.resources;CHECK(report.status==XR_XIR_OUT_OF_MEMORY || report.status==XR_XIR_BUDGET);
        XrXirStatus again=names_whole(&context,packet,source);
        if(again!=(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET))
            fprintf(stderr,"Names retry fault%zu first%u again%u\n",fault,(unsigned)report.status,(unsigned)again);
        CHECK(again==(report.status==XR_XIR_OUT_OF_MEMORY?XR_XIR_OK:XR_XIR_BUDGET));
        XrCompileResourceStats after=names_stats(&context);names_monotonic(report.spent,after);
        CHECK(context.resources==same && after.live_bytes==baseline.live_bytes && after.allocated_bytes<=caps->allocated_bytes &&
            after.peak_bytes<=caps->live_bytes && after.work<=caps->work);
    }
    xr_compile_resources_release(context.resources);
done:
    if(!report.sites)report.sites=source_fixture_compile_attempts-start;source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_fixture_compile_live==held && source_fixture_compile_bytes==held_bytes);*output=report;
}
static void names_controls(const XrXirCheckedPacket *packet,const char *source,unsigned variant,bool full) {
    NamesReport first={0},second={0};names_trial(packet,source,&names_caps,SIZE_MAX,false,&first);
    names_trial(packet,source,&names_caps,SIZE_MAX,false,&second);
    CHECK(first.status==XR_XIR_OK && second.status==XR_XIR_OK && first.sites==second.sites && first.sites>2 &&
        !memcmp(&first.spent,&second.spent,sizeof(first.spent)));
    fprintf(stderr,"Constructor names variant%u census%zu allocated=%llu peak=%llu work=%llu\n",variant,first.sites,
        (unsigned long long)first.spent.allocated_bytes,(unsigned long long)first.spent.peak_bytes,(unsigned long long)first.spent.work);fflush(stderr);
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less) {
        XrCompileResourceLimits caps=names_caps;uint64_t exact=axis==0?first.spent.allocated_bytes:axis==1?first.spent.peak_bytes:first.spent.work;CHECK(exact>1);
        if(!axis)caps.allocated_bytes=exact-less;else if(axis==1)caps.live_bytes=exact-less;else caps.work=exact-less;
        NamesReport report={0};names_trial(packet,source,&caps,SIZE_MAX,less!=0,&report);
        CHECK(report.status==(less?XR_XIR_BUDGET:XR_XIR_OK));
        if(!less)CHECK(report.sites==first.sites && !memcmp(&report.spent,&first.spent,sizeof(first.spent)));
    }
    size_t samples[]={0,first.sites/2,first.sites-1};
    for(size_t i=0;i<(full?first.sites:3);++i) {
        size_t fault=full?i:samples[i];NamesReport report={0};names_trial(packet,source,&names_caps,fault,true,&report);
        CHECK(report.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && report.sites==fault+1);
    }
    fprintf(stderr,"Constructor names variant%u %s fault cases%zu, six original axes and original-owner retry where owner exists\n",
        variant,full?"complete":"sampled",full?first.sites:(size_t)3);
}
static XrXirCheckedPacket names_library(XrXirCompileContext *transport) {
    XrXirCompileContext producer={0};CHECK(xr_compile_resources_new(&names_caps,&producer.resources)==XR_COMPILE_RESOURCE_OK);
    producer.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(producer.resources,&session)==XR_COMPILER_SESSION_OK);
    char path[2048],text[12000];int n=snprintf(path,sizeof(path),"%s/constructor_names.xr",XR_CTOR_NAMES_WORK);CHECK(n>0 && n<(int)sizeof(path));
    n=snprintf(text,sizeof(text),names_generator,generated_schema_source,XR_XIR_CONST_STRING,XR_XIR_RETURN);CHECK(n>0 && n<(int)sizeof(text));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CTOR_NAMES_WORK};
    XrXirSourceRequest request={session,path,&authority,&producer,NULL,NULL,XR_XIR_LIBRARY,NULL,NULL};
    XrXirSourceText source={"constructor_names.xr",text,(size_t)n};XrXirSourceResult result={0};
    CHECK(xr_xir_compile_source_check_text(&request,&source,&result,NULL,NULL)==XR_XIR_OK);
    memset(text,0xa5,sizeof(text));xr_compile_session_free(session);XrXirCheckedPacket original={0};
    CHECK(xr_xir_compile_checked_write(result.checked,&original,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    CHECK(xr_compile_resources_new(&names_caps,&transport->resources)==XR_COMPILE_RESOURCE_OK);transport->limits=xr_xir_compile_default_limits();
    XrXirCheckedPacket packet={0};void *owned=NULL;CHECK(xr_compile_resources_alloc(transport->resources,original.length,&owned)==XR_COMPILE_RESOURCE_OK);
    packet.bytes=owned;packet.length=original.length;memcpy(packet.bytes,original.bytes,packet.length);
    memset(original.bytes,0xa5,original.length);xr_xir_compile_checked_packet_free(&original);xr_compile_resources_release(producer.resources);return packet;
}
static size_t names_number(const char *text) {
    CHECK(text && *text);for(const char *p=text;*p;++p)CHECK(*p>='0' && *p<='9');
    errno=0;char *end=NULL;unsigned long long value=strtoull(text,&end,10);CHECK(!errno && end && !*end && value<=SIZE_MAX);return (size_t)value;
}
static int names_partition(char **argv) {
    size_t first=names_number(argv[3]),end=names_number(argv[4]),sites=names_number(argv[5]);CHECK(first<=end && end<=sites && (sites || (!first && !end)));
    FILE *file=fopen(argv[2],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>0 && length<=67108864 && !fseek(file,0,SEEK_SET));
    uint8_t *bytes=xr_malloc((size_t)length);CHECK(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length && !fclose(file));
    XrXirCheckedPacket packet={bytes,(size_t)length};char source[4096];names_case_source(XR_CTOR_NAMES_RESOURCE_VARIANT,source,sizeof(source));
    uint8_t digest[32];xr_sha256(bytes,(size_t)length,digest);FILE *report=fopen(argv[6],"wb");CHECK(report);
    CHECK(fprintf(report,"{\"event\":\"input\",\"first\":%zu,\"end\":%zu,\"sites\":%zu,\"packet_sha256\":\"",first,end,sites)>0);
    for(unsigned i=0;i<32;++i)CHECK(fprintf(report,"%02x",(unsigned)digest[i])==2);
    CHECK(fprintf(report,"\",\"caps\":[67108864,8388608,128000000]}\n")>0);
    if(!sites) {
        NamesReport census={0};names_trial(&packet,source,&names_caps,SIZE_MAX,false,&census);CHECK(census.status==XR_XIR_OK && census.owner && census.sites>2);
        CHECK(fprintf(report,"{\"event\":\"census\",\"sites\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu}\n",census.sites,
            (unsigned long long)census.spent.allocated_bytes,(unsigned long long)census.spent.peak_bytes,(unsigned long long)census.spent.work)>0);
    }else for(size_t fault=first;fault<end;++fault) {
        NamesReport trial={0};names_trial(&packet,source,&names_caps,fault,true,&trial);
        CHECK(trial.status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && trial.sites==fault+1);
        CHECK(fprintf(report,"{\"event\":\"fault\",\"ordinal\":%zu,\"attempts\":%zu,\"owner\":%u}\n",fault,trial.sites,(unsigned)trial.owner)>0);
    }
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    CHECK(fprintf(report,"{\"event\":\"complete\",\"points\":%zu,\"compiler_blocks\":0,\"compiler_bytes\":0}\n",end-first)>0 && !fclose(report));
    memset(bytes,0xa5,(size_t)length);xr_free(bytes);return 0;
}
int main(int argc,char **argv) {
    if(argc==7 && !strcmp(argv[1],"--compiler-fi"))return names_partition(argv);
    bool full=argc==2 && !strcmp(argv[1],"--resources"),controls=argc==2 && !strcmp(argv[1],"--compiler-controls");
    bool emit=argc==3 && !strcmp(argv[1],"--emit-generator");CHECK(argc==1 || full || controls || emit);
    XrXirCompileContext transport={0};XrXirCheckedPacket packet=names_library(&transport);
    if(emit) {FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    for(unsigned i=0;!emit && i<4;++i) {
        if(controls && i!=XR_CTOR_NAMES_RESOURCE_VARIANT)continue;
        char source[4096];names_case_source(i,source,sizeof(source));
        names_controls(&packet,source,i,full);memset(source,0xa5,sizeof(source));
    }
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_release(transport.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Current names Context5 four positive Source/g/Checked families, physical0; old shape refusal remains separate normal control");return 0;
}
