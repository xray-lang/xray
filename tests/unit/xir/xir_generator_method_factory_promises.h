/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_method_factory_promises.h - Creation promises remain distinct from production
 */
#ifndef XIR_GENERATOR_METHOD_FACTORY_PROMISES_H
#define XIR_GENERATOR_METHOD_FACTORY_PROMISES_H
#include "../test_win_compat.h"
static void method_factory_file(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");CHECK(file);CHECK(fputs(text,file)>=0 && !fclose(file));
}
static void method_factory_promise_execute(XrXirArtifact *checked) {
    const XrXirCompileContext context=*xr_xir_compile_artifact_context(checked);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[2]={0};const char *names[]={"run","direct"};
    for(uint32_t f=0;f<module->function_count;++f) for(unsigned n=0;n<2;++n)
        if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) ids[n]=f;
    CHECK(ids[0] && ids[1]);XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    for(unsigned n=0;n<2;++n) {
        CHECK(xr_xir_instance_start(instance,ids[n],NULL,0)==XR_XIR_CALL_READY);
        method_factory_timer(instance,5);XrXirValue value=method_factory_result(instance);
        CHECK(value.type==XR_XIR_I64 && value.payload==7);xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
}
static void generator_method_factory_promises(void) {
    char directory[]="xir-method-factory-XXXXXX",absolute[4096],entry[8192],manifest[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(entry,sizeof(entry),"%s/root.xr",absolute)>0);
    CHECK(snprintf(manifest,sizeof(manifest),"%s/xray.toml",absolute)>0);
    const char *source=
        "import time\ninterface P {iterator(value:i64)->Iterator<i64>}\n"
        "struct C implements P {iterator(value:i64=7)->Iterator<i64> {time.sleep(5)\n yield value}}\n"
        "fn make<S:P>(v:S)->Iterator<i64>{return v.iterator(7)}\n"
        "export fn run()->i64 {for(value in make(C{})){return value}\n return 0}\n"
        "export fn direct()->i64 {for(value in C{}.iterator()){return value}\n return 0}\nfn main(){}\n";
    method_factory_file(manifest,
        "[declarations]\nversion=1\n[[declarations.function]]\nmodule=\"root.xr\"\nowner=\"P\"\nname=\"iterator\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nowner=\"C\"\nname=\"iterator\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"make\"\nno_suspend=true\n");
    for(unsigned rejected=0;rejected<2;++rejected) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);method_factory_file(entry,source);
        if(rejected) {
            FILE *file=fopen(entry,"ab");CHECK(file);
            CHECK(fputs("fn bad()->i64{for(value in C{}.iterator()){return value}\n return 0}\n",file)>=0 && !fclose(file));
            file=fopen(manifest,"ab");CHECK(file);
            CHECK(fputs("[[declarations.function]]\nmodule=\"root.xr\"\nname=\"bad\"\nno_suspend=true\n",file)>=0 && !fclose(file));
        }
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
        XrXirSourceRequest request={session,entry,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);xr_compile_session_free(session);
        if(status!=(rejected ? XR_XIR_BAD_TYPE : XR_XIR_OK)) fprintf(stderr,"Method promise%u status%u line%d: %s\n",rejected,status,diagnostic.line,diagnostic.message);
        CHECK(status==(rejected ? XR_XIR_BAD_TYPE : XR_XIR_OK));
        if(rejected) CHECK(!result.checked && strstr(diagnostic.message,"declared no_suspend"));
        else {CHECK(result.checked);method_factory_promise_execute(result.checked);}
        xr_xir_compile_source_result_free(&result);source_fixture_owner_free(&owner);
    }
    CHECK(!xr_test_unlink(entry) && !xr_test_unlink(manifest) && !xr_test_rmdir(directory));
    puts("Generator method creation no_suspend, witness, default and consumer rejection PASS");
}
#endif
