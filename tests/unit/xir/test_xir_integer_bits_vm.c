/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_integer_bits_vm.c - Bit recipe runtime faults and physical ownership
 *
 * KEY CONCEPT: Expected bits are counted and permuted independently of recipes.
 */
#define main bits_unused_stdlib_main
#include "test_xir_stdlib_vm.c"
#undef main

static const char *const bits_names[]={"countBits","leading","trailing","swapped","left","right"};
static XrXirProgram *bits_program(const XrXirCompileContext *context,
    const char *root,const char *entry,uint32_t ids[6]) {
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
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    for (unsigned i=0;i<6;++i) {
        ids[i]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length==strlen(bits_names[i]) && !memcmp(function->name,bits_names[i],function->name_length)) {
                CHECK(ids[i]==UINT32_MAX);ids[i]=f;
            }
        }
        CHECK(ids[i]!=UINT32_MAX);
    }
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return program;
}
typedef struct BitsWord { uint64_t value;int64_t count; } BitsWord;
static const BitsWord bits_words[]={
    {0,0},{UINT64_MAX,-1},{UINT64_C(0x8000000000000000),INT64_MIN},{INT64_MAX,INT64_MAX},
    {UINT64_C(0x0123456789abcdef),8},{UINT64_C(0xaaaaaaaaaaaaaaaa),-65},
    {UINT64_C(0x5555555555555555),65},{2,63},{UINT64_C(0xff00),7},
    {UINT64_C(0x0102040810204080),31},{UINT64_C(0xffffffff00000000),32},{UINT64_MAX-41,-64}
};
static uint64_t bits_expected(unsigned method,const BitsWord *word) {
    uint64_t result=0;unsigned amount=(unsigned)((uint64_t)word->count&63u);
    if (method==1 || method==2) {
        for (unsigned bit=0;bit<64;++bit) {
            unsigned index=method==1 ? 63u-bit : bit;
            if ((word->value>>index)&1u) break;
            ++result;
        }
        return result;
    }
    for (unsigned bit=0;bit<64;++bit) {
        if (!((word->value>>bit)&1u)) continue;
        if (method==0) ++result;
        else {
            unsigned target=method==3 ? (7u-bit/8u)*8u+bit%8u :
                method==4 ? (bit+amount)%64u : (bit+64u-amount)%64u;
            result|=UINT64_C(1)<<target;
        }
    }
    return result;
}
static int64_t bits_payload(uint64_t value) { int64_t payload;memcpy(&payload,&value,sizeof(payload));return payload; }
static size_t bits_execute(XrXirProgram *program,uint32_t entry,
    unsigned method,const BitsWord *word,size_t failure) {
    size_t baseline_live=runtime_live,baseline_bytes=runtime_bytes;XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue arguments[]={{XR_XIR_I64,0,bits_payload(word->value)},{XR_XIR_I64,0,word->count}};
    size_t first=runtime_attempts;CHECK(failure==SIZE_MAX || failure<=SIZE_MAX-first);
    runtime_fail_at=failure==SIZE_MAX ? SIZE_MAX : first+failure;
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,arguments,method<4 ? 1 : 2);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    size_t attempts=runtime_attempts-first;runtime_fail_at=SIZE_MAX;
    if (failure==SIZE_MAX) {
        CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0},alias={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && (uint64_t)value.payload==bits_expected(method,word));
        CHECK(xr_xir_value_copy(&value,&alias)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
        CHECK(alias.type==XR_XIR_I64 && (uint64_t)alias.payload==bits_expected(method,word));xr_xir_value_drop(&alias);
    } else {
        CHECK(attempts>failure && status==XR_XIR_CALL_OOM);
        XrXirValue sentinel={XR_XIR_I64,0,77},value=sentinel;
        XrXirCallStatus transfer=xr_xir_instance_take_result(instance,&value);
        CHECK(transfer==XR_XIR_CALL_OOM || transfer==XR_XIR_CALL_BAD_STATE);
        CHECK(!memcmp(&value,&sentinel,sizeof(value)));
        XrXirCallStatus released=xr_xir_instance_free(instance);
        CHECK(released==XR_XIR_CALL_READY || released==XR_XIR_CALL_OOM);
    }
    CHECK(runtime_live==baseline_live && runtime_bytes==baseline_bytes);return attempts;
}
int main(int argc,char **argv) {
    CHECK(argc==4);bool full=!strcmp(argv[3],"faults");CHECK(full || !strcmp(argv[3],"measure"));
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint32_t ids[6];XrXirProgram *program=bits_program(&context,argv[1],argv[2],ids);size_t total=0;
    unsigned count=(unsigned)(sizeof(bits_words)/sizeof(*bits_words));
    for (unsigned i=0;i<count;++i) for (unsigned method=0;method<6;++method) {
        size_t sites=bits_execute(program,ids[method],method,&bits_words[i],SIZE_MAX);CHECK(sites);total+=sites;
        for (size_t n=0;n<(full ? sites : 2);++n) {
            size_t failure=full ? n : n ? sites-1 : 0;
            (void)bits_execute(program,ids[method],method,&bits_words[i],failure);
            printf("integer bit runtime case=%u method=%u ordinal=%zu denominator=%zu OOM physical-baseline\n",i,method,failure,sites);
        }
    }
    xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("integer bit runtime completed mode=%s cases=%u actual=%zu producer-death physical0\n",argv[3],count*6,total);return 0;
}
