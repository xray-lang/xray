/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_decimal_parse_resources.c - Whole decimal Source compiler allocation faults
 *
 * KEY CONCEPT:
 *   A failed complete trial releases every owner before a successful retry
 *   spends the same original finite ledger, without replenishing any quota.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_decimal_parse_source.h"
static const char *decimal_compile_stage;
static XrXirStatus decimal_compile_trial(const XrXirCompileContext *context) {
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;XrXirProgram *program=NULL;XrXirCSource source={0};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=XR_XIR_OK;
    decimal_compile_stage="session";XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if(created!=XR_COMPILER_SESSION_OK) {
        status=created==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
            created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;goto cleanup;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_DECIMAL_PARSE_WORK};
    XrXirSourceRequest request={session,XR_DECIMAL_PARSE_WORK "/decimal-parse.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL,NULL};
    char text[sizeof(decimal_parse_source)];memcpy(text,decimal_parse_source,sizeof(text));
    XrXirSourceText input={"decimal-parse.xr",text,sizeof(text)-1};decimal_compile_stage="Source";
    status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);memset(text,0xa5,sizeof(text));
    xr_compile_session_free(session);session=NULL;
    if(status!=XR_XIR_OK){CHECK(!result.checked && !result.snapshot);goto cleanup;}
    CHECK(result.checked && result.snapshot);decimal_compile_stage="write";
    status=xr_xir_compile_checked_write(result.checked,&packet,NULL);xr_xir_compile_source_result_free(&result);
    if(status!=XR_XIR_OK){CHECK(!packet.bytes && !packet.length);goto cleanup;}
    decimal_compile_stage="read";status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    if(status!=XR_XIR_OK){CHECK(!received);goto cleanup;}
    decimal_compile_stage="verify";status=xr_xir_compile_artifact_verify(received,NULL);if(status!=XR_XIR_OK)goto cleanup;
    decimal_compile_stage="specialize";status=xr_xir_compile_specialize(received,&closed,NULL);
    xr_xir_compile_artifact_free(received);received=NULL;
    if(status!=XR_XIR_OK){CHECK(!closed);goto cleanup;}
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};decimal_compile_stage="lower";
    status=xr_xir_compile_lower(closed,&target,&lowered,NULL);xr_xir_compile_artifact_free(closed);closed=NULL;
    if(status!=XR_XIR_OK){CHECK(!lowered);goto cleanup;}
    decimal_compile_stage="emit";status=xr_xir_compile_emit_c(lowered,"decimal_source",8388608,&source);
    if(status!=XR_XIR_OK){CHECK(!source.text && !source.length);goto cleanup;}
    CHECK(source.text && source.length);xr_xir_compile_c_source_free(&source);decimal_compile_stage="Program";
    status=xr_xir_compile_vm_program_take(&lowered,&program);
    if(status==XR_XIR_OK)CHECK(program && !lowered);else CHECK(!program);
cleanup:
    xr_xir_compile_program_drop(program);xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(received);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return status;
}
static XrXirCompileContext decimal_resource_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
int main(int argc,char **argv) {
    bool fault=argc==4 && !strcmp(argv[1],"--fault");CHECK(fault || (argc==3 && !strcmp(argv[1],"--census")));
    size_t ordinal=0;if(fault){char *end=NULL;unsigned long long value=strtoull(argv[2],&end,10);CHECK(end && !*end && value<=SIZE_MAX);ordinal=(size_t)value;}
    XrXirCompileContext context=decimal_resource_context();size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    size_t runtime_blocks=runtime_live,runtime_storage=runtime_bytes;source_fixture_compile_attempts=0;source_fixture_compile_injected=false;
    if(fault)source_fixture_compile_fail_at=ordinal;
    XrXirStatus status=decimal_compile_trial(&context);size_t sites=source_fixture_compile_attempts;source_fixture_compile_fail_at=SIZE_MAX;
    fprintf(stderr,"Decimal complete compiler fault=%u ordinal=%zu status=%u stage=%s attempts=%zu injected=%u\n",
        fault,ordinal,status,decimal_compile_stage,sites,source_fixture_compile_injected);
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes && runtime_live==runtime_blocks && runtime_bytes==runtime_storage);
    CHECK(status==(fault?XR_XIR_OUT_OF_MEMORY:XR_XIR_OK));CHECK(!fault || (source_fixture_compile_injected && sites>ordinal));
    XrXirStatus first_status=status;const char *first_stage=decimal_compile_stage;XrCompileResourceStats first={0};
    CHECK(xr_compile_resources_stats(context.resources,&first)==XR_COMPILE_RESOURCE_OK);
    if(fault){status=decimal_compile_trial(&context);CHECK(status==XR_XIR_OK);}
    CHECK(source_fixture_compile_live==live && source_fixture_compile_bytes==bytes && runtime_live==runtime_blocks && runtime_bytes==runtime_storage);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes<=67108864 && stats.peak_bytes<=8388608 && stats.work<=128000000);
    CHECK(!fault || (stats.work>first.work && stats.allocated_bytes>=first.allocated_bytes));
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    FILE *file=fopen(argv[fault?3:2],"wb");CHECK(file);
    CHECK(fprintf(file,"{\"caps\":[67108864,8388608,128000000],\"fault\":%s,\"ordinal\":%zu,\"sites\":%zu,\"status\":0,\"first_status\":%u,\"first_stage\":\"%s\",\"first_work\":%llu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"same_ledger_retry\":%s,\"compiler_blocks\":0,\"compiler_bytes\":0,\"runtime_blocks\":0,\"runtime_bytes\":0}\n",
        fault?"true":"false",ordinal,sites,first_status,first_stage,(unsigned long long)first.work,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,
        (unsigned long long)stats.work,fault?"true":"false")>0 && fclose(file)==0);return 0;
}
