/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_integer_methods_vm.c - Actual integer call closure resource ownership
 *
 * KEY CONCEPT: Every actual runtime allocation is failed independently.
 */
#define main integer_unused_stdlib_main
#include "test_xir_stdlib_vm.c"
#undef main
#include "xir/xxir_nullable.h"

static const char *const integer_entries[] = {"text","hexadecimal","checked","checkedAdd","checkedSub"};
static XrXirProgram *integer_program(const XrXirCompileContext *context,
    const char *root,const char *entry,uint32_t ids[5]) {
    XrXirLibraryCatalog *catalog = NULL;
    CHECK(xir_stdlib_library_catalog_new(context,root,&catalog) == XR_XIR_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources,&session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request = {session,entry,&authority,context,root,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(&request,&result,&diagnostic,NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0}; XrXirArtifact *decoded = NULL,*closed = NULL,*lowered = NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(closed,NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    for (unsigned i=0;i<5;++i) {
        ids[i]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length == strlen(integer_entries[i]) &&
                !memcmp(function->name,integer_entries[i],function->name_length)) {
                CHECK(ids[i] == UINT32_MAX); ids[i]=f;
            }
        }
        CHECK(ids[i] != UINT32_MAX);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program) == XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session); xr_xir_compile_library_catalog_free(catalog);
    return program;
}
typedef struct IntegerCase {
    unsigned entry; int64_t left,right,result; const char *text; bool some;
} IntegerCase;
static const IntegerCase integer_cases[] = {
    {0,INT64_MIN,0,0,"-9223372036854775808",false},
    {1,INT64_MIN,0,0,"-0x8000000000000000",false},
    {1,0,0,0,"0x0",false},
    {2,7,6,42,NULL,true}, {2,INT64_MIN,-1,0,NULL,false},
    {3,INT64_MAX,1,0,NULL,false}, {3,-7,6,-1,NULL,true},
    {4,INT64_MIN,1,0,NULL,false}, {4,7,6,1,NULL,true}
};
static void integer_result(const IntegerCase *test,const XrXirValue *value) {
    if (test->text) {
        const char *bytes=NULL; size_t length=0;
        CHECK(xr_xir_string_view(value,&bytes,&length));
        CHECK(length == strlen(test->text) && !memcmp(bytes,test->text,length));
    } else {
        bool some=false; const XrXirValue *payload=NULL;
        CHECK(xr_xir_nullable_view(value,&some,&payload) && some == test->some);
        CHECK(some ? payload && payload->type == XR_XIR_I64 && payload->payload == test->result : !payload);
    }
}
static size_t integer_execute(XrXirProgram *program,uint32_t entry,
    const IntegerCase *test,size_t failure) {
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY);
    XrXirValue arguments[]={{XR_XIR_I64,0,test->left},{XR_XIR_I64,0,test->right}};
    size_t first=runtime_attempts;
    CHECK(failure == SIZE_MAX || failure <= SIZE_MAX-first);
    runtime_fail_at=failure == SIZE_MAX ? SIZE_MAX : first+failure;
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arguments,test->entry < 2 ? 1 : 2);
    if (status == XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    size_t attempts=runtime_attempts-first;
    runtime_fail_at=SIZE_MAX;
    XrXirValue value={0},alias={0};
    if (failure == SIZE_MAX) {
        CHECK(status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value) == XR_XIR_CALL_RETURNED);
        integer_result(test,&value);
        CHECK(xr_xir_value_copy(&value,&alias) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        /* Returned owners remain valid after the producer Instance dies. */
        xr_xir_value_drop(&value); integer_result(test,&alias); xr_xir_value_drop(&alias);
    } else {
        CHECK(attempts > failure && status == XR_XIR_CALL_OOM);
        XrXirValue sentinel={XR_XIR_I64,0,77}; value=sentinel;
        XrXirCallStatus transfer=xr_xir_instance_take_result(instance,&value);
        CHECK(transfer == XR_XIR_CALL_OOM || transfer == XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&value,&sentinel,sizeof(value)));
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released == XR_XIR_CALL_READY || released == XR_XIR_CALL_OOM);
    }
    CHECK(runtime_live == baseline_live && runtime_bytes == baseline_bytes);
    return attempts;
}
int main(int argc,char **argv) {
    CHECK(argc == 4); bool full=!strcmp(argv[3],"faults");
    CHECK(full || !strcmp(argv[3],"measure"));
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0}; context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources) == XR_COMPILE_RESOURCE_OK);
    uint32_t ids[5]; XrXirProgram *program=integer_program(&context,argv[1],argv[2],ids);
    size_t total=0;
    for (unsigned i=0;i<sizeof(integer_cases)/sizeof(*integer_cases);++i) {
        const IntegerCase *test=&integer_cases[i];
        size_t sites=integer_execute(program,ids[test->entry],test,SIZE_MAX); CHECK(sites);
        total+=sites;
        for (size_t n=0;n<(full ? sites : 2);++n) {
            size_t failure=full ? n : n ? sites-1 : 0;
            (void)integer_execute(program,ids[test->entry],test,failure);
            printf("integer runtime case=%u ordinal=%zu denominator=%zu OOM physical-baseline\n",i,failure,sites);
        }
    }
    xr_xir_compile_program_drop(program); xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("integer runtime completed mode=%s cases=9 actual=%zu producer-death physical0\n",argv[3],total);
    return 0;
}
