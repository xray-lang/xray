/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_scalar_methods_vm.c - Actual scalar call faults and returned owners
 *
 * KEY CONCEPT: Original bits and physical allocation sites remain observable.
 */
#define main scalar_unused_stdlib_main
#include "test_xir_stdlib_vm.c"
#undef main

static const char *const scalar_names[]={"magnitude","integral","floorValue","ceilValue",
    "roundValue","notNumber","floatingText","booleanText","runeText","codepoint"};
static XrXirProgram *scalar_program(const XrXirCompileContext *context,
    const char *root,const char *entry,uint32_t ids[10]) {
    XrXirLibraryCatalog *catalog=NULL; XrCompilerSession *session=NULL;
    CHECK(xir_stdlib_library_catalog_new(context,root,&catalog)==XR_XIR_OK);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0}; XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    for (unsigned i=0;i<10;++i) {
        ids[i]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length==strlen(scalar_names[i]) && !memcmp(function->name,scalar_names[i],function->name_length)) {
                CHECK(ids[i]==UINT32_MAX);ids[i]=f;
            }
        }
        CHECK(ids[i]!=UINT32_MAX);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    return program;
}
typedef struct ScalarCase {
    unsigned entry; XrXirType input_type,result_type; uint64_t input,result;
    const char *text; size_t length; XrXirCallStatus status;
} ScalarCase;
static const ScalarCase scalar_cases[]={
    {0,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x8000000000000000),0,NULL,0,XR_XIR_CALL_RETURNED},
    {0,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x7ff0000000000001),UINT64_C(0x7ff8000000000000),NULL,0,XR_XIR_CALL_RETURNED},
    {0,XR_XIR_F64,XR_XIR_F64,UINT64_C(0xfff0000000000000),UINT64_C(0x7ff0000000000000),NULL,0,XR_XIR_CALL_RETURNED},
    {5,XR_XIR_F64,XR_XIR_BOOL,UINT64_C(0x7ff0000000000001),1,NULL,0,XR_XIR_CALL_RETURNED},
    {1,XR_XIR_F64,XR_XIR_I64,UINT64_C(0xbff8000000000000),UINT64_MAX,NULL,0,XR_XIR_CALL_RETURNED},
    {2,XR_XIR_F64,XR_XIR_I64,UINT64_C(0xbff8000000000000),UINT64_MAX-1,NULL,0,XR_XIR_CALL_RETURNED},
    {3,XR_XIR_F64,XR_XIR_I64,UINT64_C(0xbff8000000000000),UINT64_MAX,NULL,0,XR_XIR_CALL_RETURNED},
    {4,XR_XIR_F64,XR_XIR_I64,UINT64_C(0x4330000000000001),UINT64_C(4503599627370497),NULL,0,XR_XIR_CALL_RETURNED},
    {6,XR_XIR_F64,XR_XIR_STRING,UINT64_C(0x4045000000000000),0,"42.0",4,XR_XIR_CALL_RETURNED},
    {7,XR_XIR_BOOL,XR_XIR_STRING,1,0,"true",4,XR_XIR_CALL_RETURNED},
    {7,XR_XIR_BOOL,XR_XIR_STRING,0,0,"false",5,XR_XIR_CALL_RETURNED},
    {8,XR_XIR_RUNE,XR_XIR_STRING,0,0,"\0",1,XR_XIR_CALL_RETURNED},
    {8,XR_XIR_RUNE,XR_XIR_STRING,0x1f600,0,"\xf0\x9f\x98\x80",4,XR_XIR_CALL_RETURNED},
    {9,XR_XIR_RUNE,XR_XIR_U32,0x10ffff,0x10ffff,NULL,0,XR_XIR_CALL_RETURNED},
    {1,XR_XIR_F64,XR_XIR_UNIT,UINT64_C(0x7ff0000000000001),0,NULL,0,XR_XIR_CALL_NUMERIC_RANGE},
    {2,XR_XIR_F64,XR_XIR_UNIT,UINT64_C(0x7ff0000000000000),0,NULL,0,XR_XIR_CALL_NUMERIC_RANGE},
    {3,XR_XIR_F64,XR_XIR_UNIT,UINT64_C(0x43e0000000000000),0,NULL,0,XR_XIR_CALL_NUMERIC_RANGE},
    {4,XR_XIR_F64,XR_XIR_UNIT,UINT64_C(0xc3e0000000000001),0,NULL,0,XR_XIR_CALL_NUMERIC_RANGE}
};
static int64_t scalar_payload(uint64_t bits) {
    return bits<=INT64_MAX ? (int64_t)bits : -1-(int64_t)(UINT64_MAX-bits);
}
static void scalar_result(const ScalarCase *test,const XrXirValue *value) {
    CHECK(value->type==(uint32_t)test->result_type);
    if (test->text) {
        const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(value,&bytes,&length) && length==test->length && !memcmp(bytes,test->text,length));
    } else CHECK(value->payload==scalar_payload(test->result));
}
static size_t scalar_execute(XrXirProgram *program,uint32_t entry,
    const ScalarCase *test,size_t failure) {
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue argument={test->input_type,0,scalar_payload(test->input)};
    size_t first=runtime_attempts;CHECK(failure==SIZE_MAX || failure<=SIZE_MAX-first);
    runtime_fail_at=failure==SIZE_MAX ? SIZE_MAX : first+failure;
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,&argument,1);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    size_t attempts=runtime_attempts-first;runtime_fail_at=SIZE_MAX;
    if (failure==SIZE_MAX && test->status==XR_XIR_CALL_RETURNED) {
        CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0},alias={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);scalar_result(test,&value);
        CHECK(xr_xir_value_copy(&value,&alias)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&value);scalar_result(test,&alias);xr_xir_value_drop(&alias);
    } else {
        CHECK(failure==SIZE_MAX ? status==test->status : attempts>failure && status==XR_XIR_CALL_OOM);
        XrXirValue sentinel={XR_XIR_I64,0,77},value=sentinel;
        XrXirCallStatus transfer=xr_xir_instance_take_result(instance,&value);
        if (failure==SIZE_MAX && test->status==XR_XIR_CALL_NUMERIC_RANGE) {
            /* Root language panics are observed by poll, not successful value
             * transfer. Occupied output rejects before the Call state check. */
            CHECK(transfer==XR_XIR_CALL_BAD_ARGUMENT);
            XrXirValue empty={0};
            CHECK(xr_xir_instance_take_result(instance,&empty)==XR_XIR_CALL_BAD_STATE);
            CHECK(!empty.type && !empty.reserved && !empty.payload);
        } else CHECK(transfer==status || transfer==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&value,&sentinel,sizeof(value)));
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released==XR_XIR_CALL_READY || released==status);
    }
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);return attempts;
}
int main(int argc,char **argv) {
    CHECK(argc==4);bool full=!strcmp(argv[3],"faults");CHECK(full || !strcmp(argv[3],"measure"));
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint32_t ids[10];XrXirProgram *program=scalar_program(&context,argv[1],argv[2],ids);size_t total=0;
    unsigned count=(unsigned)(sizeof(scalar_cases)/sizeof(*scalar_cases));
    for (unsigned i=0;i<count;++i) {
        const ScalarCase *test=&scalar_cases[i];size_t sites=scalar_execute(program,ids[test->entry],test,SIZE_MAX);CHECK(sites);
        total+=sites;
        for (size_t n=0;n<(full ? sites : 2);++n) {
            size_t failure=full ? n : n ? sites-1 : 0;(void)scalar_execute(program,ids[test->entry],test,failure);
            printf("scalar runtime case=%u ordinal=%zu denominator=%zu OOM physical-baseline\n",i,failure,sites);
        }
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("scalar runtime completed mode=%s cases=%u actual=%zu producer-death physical0\n",argv[3],count,total);return 0;
}
