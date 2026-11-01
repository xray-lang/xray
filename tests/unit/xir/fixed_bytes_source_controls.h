/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixed_bytes_source_controls.h - Exact annotations and owned cold library bytes
 *
 * KEY CONCEPT:
 *   An authenticated cold library retains the fixed length without source files.
 */
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xcompile_request.h"
static uint64_t fixed_held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
typedef struct FixedBytesCancel {uint32_t polls,stop;} FixedBytesCancel;
static bool fixed_cancel_at(const void *owner) {
    FixedBytesCancel *state=(FixedBytesCancel *)owner;return ++state->polls==state->stop;
}
static XrXirStatus fixed_source_text(const XrXirCompileContext *context,const char *text,
    const char *name,XrXirLinkageKind linkage,const XrXirLibraryCatalog *catalog,
    XrCompileRequestControl *control,XrXirSourceResult *output) {
    char path[2048];int size=snprintf(path,sizeof(path),"%s/%s",XR_FIXED_BYTES_ROOT,name);
    CHECK(size>0 && (size_t)size<sizeof(path));XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FIXED_BYTES_ROOT};
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,
        .context=context,.linkage_kind=linkage,.libraries=catalog};
    XrXirSourceText input={path,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"fixed text status%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void fixed_source_controls(const XrXirCompileContext *context) {
    const char *sources[]={
        "const x:[u8;2]=b\"AB\"\nassert(len(x)==2 && x[0]==65)\n",
        "const x:[u8;3]=b\"AB\"\n",
        "const x:Array<u8>=b\"AB\"\n",
        "const x:[i64;2]=b\"AB\"\n",
        "const x=b\"\"\n",
        "const x:[u8;0]=b\"A\"\n",
        "const x:[u8;65536]=b\"A\"\n",
        "var x=b\"AB\"\nx[0]=1\n",
        "const x=c\"A\\0B\"\n"};
    uint64_t held=fixed_held(context);
    for(unsigned i=0;i<sizeof(sources)/sizeof(*sources);++i) {
        XrXirSourceResult result={0};
        XrXirStatus status=fixed_source_text(context,sources[i],"fixed-control.xr",
            XR_XIR_PROGRAM,NULL,NULL,&result);
        CHECK((status==XR_XIR_OK)==!i && (result.checked!=NULL)==!i);
        xr_xir_compile_source_result_free(&result);CHECK(fixed_held(context)==held);
    }
    FixedBytesCancel cancelled={0,1};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={fixed_cancel_at,&cancelled,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceResult result={0};
    CHECK(fixed_source_text(context,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        &control,&result)==XR_XIR_CANCELLED && !result.checked && cancelled.polls);
    CHECK(fixed_held(context)==held);
    XrXirCompileContext bounded=*context;bounded.limits.instructions=1;
    CHECK(fixed_source_text(&bounded,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        NULL,&result)==XR_XIR_BUDGET && !result.checked && fixed_held(context)==held);
    CHECK(fixed_source_text(context,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        NULL,&result)==XR_XIR_OK && result.checked);
    xr_xir_compile_source_result_free(&result);CHECK(fixed_held(context)==held);
}
static void fixed_cold_library(const XrXirCompileContext *context) {
    uint64_t held=fixed_held(context);XrXirSourceResult producer={0};
    CHECK(fixed_source_text(context,"export fn magic()->[u8;2]{return b\"AB\"}\n",
        "fixed-lib.xr",XR_XIR_LIBRARY,NULL,NULL,&producer)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FIXED_BYTES_ROOT};
    XrXirLibraryModuleInput binding={authority,"fixed-lib.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,
        .modules=&binding,.module_count=1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirSourceResult consumer={0};
    CHECK(fixed_source_text(context,"import {magic} from \"./fixed-lib\"\n"
        "const x=magic()\nassert(len(x)==2 && x[1]==66)\n",
        "fixed-main.xr",XR_XIR_PROGRAM,catalog,NULL,&consumer)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);catalog=NULL;
    XrXirArtifact *special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(consumer.checked,&special,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&consumer);
    CHECK(xr_xir_compile_lower(special,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entry=module->declarations->entry_function;XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(unsigned i=0;i<1000 && status==XR_XIR_CALL_READY;++i)
        status=xr_xir_instance_poll_bounded(instance,1000).outcome.status;
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(fixed_held(context)==held);
}
