/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derived_records.c - Ordinary constant table recipes and receiving
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include <ctype.h>
#include "xir_derived_records_source.h"
#include "xir/xxir_emit_c.h"
static void derived_records_table(void) {
    XrXirCompileContext context={0};XrXirArtifact *lowered=derived_records_lower(&context);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t checksum_entry=derived_records_function(module,"checksum"),table_entry=derived_records_function(module,"table");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,checksum_entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue checksum={0};CHECK(xr_xir_instance_take_result(instance,&checksum)==XR_XIR_CALL_RETURNED);
    CHECK(checksum.type==XR_XIR_I64 && !checksum.reserved && checksum.payload==649367177924771840);
    xr_xir_value_drop(&checksum);
    CHECK(xr_xir_instance_start(instance,table_entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1024).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue table={0};CHECK(xr_xir_instance_take_result(instance,&table)==XR_XIR_CALL_RETURNED);
    derived_records_value(&table,reader);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    xr_compile_resources_release(context.resources);context.resources=NULL;
    derived_records_value(&table,reader);xr_xir_value_drop(&table);xr_xir_domain_drop(reader);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void derived_records_refusals(void) {
    const char *cases[]={
        "fn large()->Array<i8>{return [1,2,3,4,5,6,7,8,9]}\nexport fn table()->Array<i8>{return comptime large()}\n",
        "fn effectful()->Array<i8>{print(1);return [1,2]}\nexport fn table()->Array<i8>{return comptime effectful()}\n"};
    const XrXirStatus expected[]={XR_XIR_BUDGET,XR_XIR_BAD_TYPE};
    for(unsigned i=0;i<2;++i) {
        XrXirCompileContext context={0};
        CHECK(xr_compile_resources_new(&derived_records_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=derived_records_source(&context,cases[i],&source,&diagnostic);
        CHECK(status==expected[i] && diagnostic.status==expected[i] && diagnostic.line>0);
        CHECK(!source.checked && !source.snapshot);xr_compile_resources_release(context.resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}
static XrCompileResourceStats derived_records_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void derived_records_resources(void) {
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&derived_records_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline=derived_records_stats(&context);
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    source_fixture_compile_attempts=0;
    CHECK(derived_records_source(&context,derived_records_table_source,&source,&diagnostic)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts;XrCompileResourceStats required=derived_records_stats(&context);
    xr_xir_compile_source_result_free(&source);
    CHECK(derived_records_stats(&context).live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);
    CHECK(sites && !source_fixture_compile_live && !source_fixture_compile_bytes);
    for(size_t fault=0;fault<sites;++fault) {
        context=(XrXirCompileContext){0};
        CHECK(xr_compile_resources_new(&derived_records_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();baseline=derived_records_stats(&context);
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=fault;
        source=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=derived_records_source(&context,derived_records_table_source,&source,&diagnostic);
        source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !source.checked && !source.snapshot);
        CHECK(source_fixture_compile_attempts>fault && derived_records_stats(&context).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=derived_records_stats(&context);
        CHECK(derived_records_source(&context,derived_records_table_source,&source,&diagnostic)==XR_XIR_OK && source.checked && source.snapshot);
        xr_xir_compile_source_result_free(&source);XrCompileResourceStats retry=derived_records_stats(&context);
        CHECK(retry.live_bytes==baseline.live_bytes && retry.allocated_bytes>=failed.allocated_bytes && retry.work>=failed.work);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits limits=derived_records_limits;
        if(axis==0)limits.allocated_bytes=required.allocated_bytes-shortfall;
        if(axis==1)limits.live_bytes=required.peak_bytes-shortfall;
        if(axis==2)limits.work=required.work-shortfall;
        context=(XrXirCompileContext){0};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();baseline=derived_records_stats(&context);
        source=(XrXirSourceResult){0};diagnostic=(XrXirSourceDiagnostic){0};
        XrXirStatus status=derived_records_source(&context,derived_records_table_source,&source,&diagnostic);
        CHECK(status==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?(!source.checked && !source.snapshot):(source.checked && source.snapshot));
        xr_xir_compile_source_result_free(&source);CHECK(derived_records_stats(&context).live_bytes==baseline.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    printf("Constant table whole Source: %zu allocation faults with same-ledger retry, three exact/minus-one axes, physical0; allocated=%llu peak=%llu work=%llu\n",
        sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
static void derived_records_runtime_resources(void) {
    XrXirCompileContext context={0};XrXirArtifact *lowered=derived_records_lower(&context);
    XrXirDomain *reader=NULL;CHECK(xr_xir_compile_domain_new(context.resources,UINT64_MAX,&reader)==XR_XIR_VALUE_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[2]={derived_records_function(module,"checksum"),derived_records_function(module,"table")};
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    size_t sites=derived_records_runtime_failures(program,entries,reader,true);
    xr_xir_compile_program_drop(program);xr_xir_domain_drop(reader);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    printf("Ordinary constant table VM: %zu runtime allocation faults, same Program retry, two entries and physical0\n",sites);
}
static void derived_records_emit(const char *path) {
    XrXirCompileContext context={0};XrXirArtifact *lowered=derived_records_lower(&context);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entries[2]={derived_records_function(module,"checksum"),derived_records_function(module,"table")};
    XrXirCSource emitted={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_records",8388608,&emitted)==XR_XIR_OK);
    /* GNU statement expressions are forbidden even when a provider accepts them. */
    for(size_t i=0;i<emitted.length;++i)if(emitted.text[i]=='(') {
        size_t next=i+1;
        while(next<emitted.length && isspace((unsigned char)emitted.text[next]))++next;
        CHECK(next==emitted.length || emitted.text[next]!='{');
    }
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(emitted.text,1,emitted.length,file)==emitted.length);
    CHECK(fprintf(file,"\nconst uint32_t fixture_records_selection[2]={%uu,%uu};\n",entries[0],entries[1])>0);
    CHECK(!fclose(file));printf("Ordinary constant table generated C: %zu bytes / %u functions\n",emitted.length,module->function_count);
    xr_xir_compile_c_source_free(&emitted);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--resources")){derived_records_resources();return 0;}
    if(argc==2 && !strcmp(argv[1],"--runtime-resources")){derived_records_runtime_resources();return 0;}
    if(argc==3 && !strcmp(argv[1],"--emit-native")){derived_records_emit(argv[2]);return 0;}
    CHECK(argc==1);derived_records_table();derived_records_refusals();source_fixture_source_owners_free();
    puts("Exact named record oracle: index/square fields, independent loop recipe, packet, capacity4, all8 field bytes, producer death and output/effect refusals");return 0;
}
