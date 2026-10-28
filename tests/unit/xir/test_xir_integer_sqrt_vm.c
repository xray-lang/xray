/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_integer_sqrt_vm.c - Square root values, allocation faults and lifetime
 *
 * KEY CONCEPT: Exact binary64 golden values survive all producer destruction.
 */
#define main sqrt_unused_stdlib_main
#include "test_xir_stdlib_vm.c"
#undef main

typedef struct SqrtCase { int64_t input;uint64_t expected; } SqrtCase;
static const SqrtCase sqrt_cases[]={
    {INT64_MIN,UINT64_C(0x7ff8000000000000)}, {-1,UINT64_C(0x7ff8000000000000)},
    {0,0}, {1,UINT64_C(0x3ff0000000000000)}, {2,UINT64_C(0x3ff6a09e667f3bcd)},
    {3,UINT64_C(0x3ffbb67ae8584caa)},
    {INT64_C(9007199254740991),UINT64_C(0x4196a09e667f3bcc)},
    {INT64_C(9007199254740993),UINT64_C(0x4196a09e667f3bcd)},
    {INT64_MAX,UINT64_C(0x41e6a09e667f3bcd)}
};

static XrXirProgram *sqrt_program(const XrXirCompileContext *context,
    const char *root,const char *entry,uint32_t *identity) {
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
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    *identity=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length==10 && !memcmp(function->name,"squareRoot",10)) {
            CHECK(*identity==UINT32_MAX);*identity=f;
        }
    }
    CHECK(*identity!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return program;
}

static size_t sqrt_execute(XrXirProgram *program,uint32_t entry,const SqrtCase *test,
    size_t failure,XrXirValue *alias) {
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue argument={XR_XIR_I64,0,test->input};
    size_t first=runtime_attempts;CHECK(failure==SIZE_MAX || failure<=SIZE_MAX-first);
    runtime_fail_at=failure==SIZE_MAX ? SIZE_MAX : first+failure;
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,&argument,1);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    size_t sites=runtime_attempts-first;runtime_fail_at=SIZE_MAX;
    if (failure==SIZE_MAX) {
        CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_F64 && (uint64_t)value.payload==test->expected);
        CHECK(alias && xr_xir_value_copy(&value,alias)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
    } else {
        CHECK(sites>failure && status==XR_XIR_CALL_OOM);
        XrXirValue sentinel={XR_XIR_I64,0,77},value=sentinel;
        XrXirCallStatus transfer=xr_xir_instance_take_result(instance,&value);
        CHECK(transfer==XR_XIR_CALL_OOM || transfer==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&value,&sentinel,sizeof(value)));
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released==XR_XIR_CALL_READY || released==XR_XIR_CALL_OOM);
    }
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);return sites;
}

int main(int argc,char **argv) {
    CHECK(argc==4);bool full=!strcmp(argv[3],"faults");CHECK(full || !strcmp(argv[3],"measure"));
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint32_t identity;XrXirProgram *program=sqrt_program(&context,argv[1],argv[2],&identity);
    XrXirValue aliases[sizeof(sqrt_cases)/sizeof(*sqrt_cases)]={0};size_t total=0;
    for (unsigned i=0;i<sizeof(sqrt_cases)/sizeof(*sqrt_cases);++i) {
        size_t sites=sqrt_execute(program,identity,&sqrt_cases[i],SIZE_MAX,&aliases[i]);CHECK(sites);total+=sites;
        for (size_t n=0;n<(full ? sites : 2);++n) {
            size_t failure=full ? n : n ? sites-1 : 0;
            (void)sqrt_execute(program,identity,&sqrt_cases[i],failure,NULL);
            printf("integer sqrt runtime case=%u ordinal=%zu denominator=%zu OOM physical-baseline\n",i,failure,sites);
        }
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<sizeof(sqrt_cases)/sizeof(*sqrt_cases);++i) {
        CHECK(aliases[i].type==XR_XIR_F64 && (uint64_t)aliases[i].payload==sqrt_cases[i].expected);
        xr_xir_value_drop(&aliases[i]);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("integer sqrt runtime completed mode=%s cases=%zu actual=%zu producer-death physical0\n",argv[3],sizeof(sqrt_cases)/sizeof(*sqrt_cases),total);
    return 0;
}
