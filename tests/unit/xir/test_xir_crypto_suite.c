/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_crypto_suite.c - Own, discover, emit and execute seven full crypto inputs
 *
 * KEY CONCEPT:
 *   The same received SourceProduct supplies catalog IDs, VM code and native C.
 *   Session and Source owners retire before any original test body executes.
 */
#include "program/xr_xir_source_product.h"
#include "base/xresource_limits.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "crypto_suite_runtime.h"
#include <string.h>
static void source_digest(const char *path,char output[65]) {
    FILE *file=fopen(path,"rb");CHECK(file);XrSHA256Context sha;xr_sha256_init(&sha);
    uint8_t buffer[4096],digest[32];size_t count;
    while((count=fread(buffer,1,sizeof(buffer),file)))xr_sha256_update(&sha,buffer,count);
    CHECK(!ferror(file)&&!fclose(file));xr_sha256_final(&sha,digest);
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){output[i*2]=hex[digest[i]>>4];output[i*2+1]=hex[digest[i]&15];}
    output[64]=0;
}
int main(int argc,char **argv) {
    CHECK(argc==4 || (argc==5 && !strcmp(argv[4],"--emit-only")));
    bool execute=argc==4;uint32_t passed_total=0,discovered=0,source_failures=0;
    FILE *report=fopen(argv[3],"wb");CHECK(report);CHECK(fputs("{\"schema\":1,\"suites\":[",report)>=0);
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    for(unsigned s=0;s<7;++s) {
        const CryptoSuite *suite=crypto_suites+s;char source[2048],stdlib[2048],path[2048],digest[65];
        CHECK(snprintf(source,sizeof(source),"%s/%s",argv[1],suite->path)>0);
        CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",argv[1])>0);
        source_digest(source,digest);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,argv[1]};
        XrXirSourceProductRequest request={{session,source,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
        if(status!=XR_XIR_OK) {
            XrFrontendRoot error={0};
            if(diagnostic.failure.report)xr_frontend_report_root(diagnostic.failure.report,0,&error);
            fprintf(stderr,"%s status=%u stage=%u %d:%d %s\n",suite->path,status,diagnostic.stage,
                error.primary.line,error.primary.column,error.message?error.message:"");
        }
        if(status!=XR_XIR_OK) {
            CHECK(!product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
            xr_compile_session_free(session);++source_failures;
            CHECK(fprintf(report,"%s{\"source\":\"%s\",\"sourceSha256\":\"%s\",\"sourceStatus\":%u}",
                s?",":"",suite->path,digest,status)>0);
            continue;
        }
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        xr_compile_session_free(session);
        const XrXirSourceTests *tests=xr_xir_compile_source_product_tests(product);
        CHECK(tests && tests->count==suite->count);uint32_t functions[14];bool passed[14]={false};
        for(uint32_t i=0;i<suite->count;++i) {
            const XrXirSourceTestEntry *test=tests->entries+i;
            CHECK(test->name_length==strlen(suite->names[i]) && !strcmp(test->name,suite->names[i]));
            CHECK(test->role==XR_XIR_TEST_ROLE_TEST && !test->timeout_seconds);
            functions[i]=test->function;
            for(uint32_t j=0;j<i;++j)CHECK(functions[j]!=functions[i]);
        }
        discovered+=tests->count;
        XrXirCSource code={0};
        CHECK(xr_xir_compile_source_product_emit(product,suite->prefix,33554432,&code)==XR_XIR_OK);
        CHECK(snprintf(path,sizeof(path),"%s/%s.c",argv[2],suite->prefix)>0);
        FILE *file=fopen(path,"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t %s_functions[%u]={",suite->prefix,suite->count)>0);
        for(uint32_t i=0;i<suite->count;++i)CHECK(fprintf(file,"%s%u",i?",":"",functions[i])>0);
        CHECK(fprintf(file,"};\nconst char %s_source_sha256[65]=\"%s\";\n",suite->prefix,digest)>0);
        CHECK(!fclose(file));xr_xir_compile_c_source_free(&code);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
        xr_xir_compile_source_product_free(product);
        if(execute)for(uint32_t i=0;i<suite->count;++i) {
            passed[i]=crypto_suite_run(program,functions[i],"VM",suite->names[i]);
            if(passed[i])++passed_total;
        }
        xr_xir_compile_program_drop(program);
        crypto_suite_report(report,suite,digest,functions,passed,execute,s);
    }
    xr_compile_resources_release(resources);
    CHECK(fprintf(report,"],\"discovered\":%u,\"executed\":%u,\"passed\":%u,\"skipped\":0,\"failed\":%u}\n",
        discovered,execute?discovered:0,passed_total,execute?discovered-passed_total:0)>0 && !fclose(report));
    CHECK(!source_failures && discovered==67);
    if(execute) {printf("crypto catalog: %u PASS, 0 SKIP, %u FAIL\n",passed_total,67-passed_total);CHECK(passed_total==67);}
    return 0;
}
