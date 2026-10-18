/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_owned_vm.c - Owned string results and actual runtime faults
 *
 * KEY CONCEPT: Returned strings survive both argument and Instance owners.
 */
#define main string_unused_stdlib_main
#include "test_xir_stdlib_vm.c"
#undef main
static XrXirProgram *string_program(const XrXirCompileContext *context,
    const char *root,const char *entry,uint32_t ids[2]) {
    XrXirLibraryCatalog *catalog=NULL;XrCompilerSession *session=NULL;
    CHECK(xir_stdlib_library_catalog_new(context,root,&catalog)==XR_XIR_OK);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const char *names[]={"copied","repeated"};
    for (unsigned i=0;i<2;++i) {
        ids[i]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *body=&module->functions[f];
            if (body->name_length==strlen(names[i]) && !memcmp(body->name,names[i],body->name_length)) {
                CHECK(ids[i]==UINT32_MAX);ids[i]=f;
            }
        }
        CHECK(ids[i]!=UINT32_MAX);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return program;
}
typedef struct StringCase { const char *bytes;size_t length;int64_t count; } StringCase;
static const StringCase string_cases[]={
    {"",0,INT64_MAX},{"a",1,INT64_MIN},{"ab",2,-1},{"ab",2,0},{"ab",2,1},
    {"ab",2,3},{"ab",2,16},{"a\0b",3,7},{"\xc3\xa9\xf0\x9f\x98\x80",6,8},{"x",1,65}
};
static void string_expected(const XrXirValue *value,const StringCase *test,bool repeat) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length));
    size_t count=repeat ? test->count>0 ? (size_t)test->count : 0 : 1;
    CHECK(!test->length || count<=SIZE_MAX/test->length);
    CHECK(length==test->length*count);
    if (test->length) for (size_t i=0;i<count;++i) CHECK(!memcmp(bytes+i*test->length,test->bytes,test->length));
}
static size_t string_execute(XrXirProgram *program,uint32_t entry,
    const StringCase *test,bool repeat,size_t failure) {
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;
    XrXirDomain *domain=NULL;XrXirValue argument={0};
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,test->bytes,test->length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={argument,{XR_XIR_I64,0,test->count}};
    size_t first=runtime_attempts;CHECK(failure==SIZE_MAX || failure<=SIZE_MAX-first);
    runtime_fail_at=failure==SIZE_MAX ? SIZE_MAX : first+failure;
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arguments,repeat ? 2 : 1);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    size_t attempts=runtime_attempts-first;runtime_fail_at=SIZE_MAX;
    if (failure==SIZE_MAX) {
        CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0},alias={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);string_expected(&value,test,repeat);
        CHECK(xr_xir_value_copy(&value,&alias)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);xr_xir_value_drop(&value);
        string_expected(&alias,test,repeat);xr_xir_value_drop(&alias);
    } else {
        CHECK(attempts>failure && status==XR_XIR_CALL_OOM);
        XrXirValue sentinel={XR_XIR_I64,0,77},value=sentinel;
        XrXirCallStatus transfer=xr_xir_instance_take_result(instance,&value);
        CHECK(transfer==XR_XIR_CALL_OOM || transfer==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&value,&sentinel,sizeof(value)));
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released==XR_XIR_CALL_READY || released==XR_XIR_CALL_OOM);
        xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    }
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);return attempts;
}
int main(int argc,char **argv) {
    CHECK(argc==4);bool full=!strcmp(argv[3],"faults");CHECK(full || !strcmp(argv[3],"measure"));
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint32_t ids[2];XrXirProgram *program=string_program(&context,argv[1],argv[2],ids);size_t total=0;
    unsigned count=(unsigned)(sizeof(string_cases)/sizeof(*string_cases));
    for (unsigned i=0;i<count;++i) for (unsigned method=0;method<2;++method) {
        size_t sites=string_execute(program,ids[method],&string_cases[i],method!=0,SIZE_MAX);CHECK(sites);total+=sites;
        for (size_t n=0;n<(full ? sites : 2);++n) {
            size_t failure=full ? n : n ? sites-1 : 0;
            (void)string_execute(program,ids[method],&string_cases[i],method!=0,failure);
            printf("string runtime case=%u method=%u ordinal=%zu denominator=%zu OOM physical-baseline\n",i,method,failure,sites);
        }
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("string runtime completed mode=%s cases=%u actual=%zu argument-and-instance-death physical0\n",argv[3],count*2,total);return 0;
}
