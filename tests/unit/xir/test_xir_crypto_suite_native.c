/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_crypto_suite_native.c - Execute the same complete catalog in generated C
 *
 * KEY CONCEPT:
 *   Each linked Program retains its real catalog role and exact received IDs.
 *   Generated code, not a host replacement, executes every crypto test body.
 */
#include "base/xresource_limits.h"
#include "crypto_suite_runtime.h"
extern const XrXirProgramSpec crypto_suite_0_program;
extern const uint32_t crypto_suite_0_functions[];
extern const char crypto_suite_0_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_1_program;
extern const uint32_t crypto_suite_1_functions[];
extern const char crypto_suite_1_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_2_program;
extern const uint32_t crypto_suite_2_functions[];
extern const char crypto_suite_2_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_3_program;
extern const uint32_t crypto_suite_3_functions[];
extern const char crypto_suite_3_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_4_program;
extern const uint32_t crypto_suite_4_functions[];
extern const char crypto_suite_4_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_5_program;
extern const uint32_t crypto_suite_5_functions[];
extern const char crypto_suite_5_source_sha256[65];
extern const XrXirProgramSpec crypto_suite_6_program;
extern const uint32_t crypto_suite_6_functions[];
extern const char crypto_suite_6_source_sha256[65];
static const XrXirProgramSpec *const programs[7]={&crypto_suite_0_program,&crypto_suite_1_program,&crypto_suite_2_program,&crypto_suite_3_program,&crypto_suite_4_program,&crypto_suite_5_program,&crypto_suite_6_program};
static const uint32_t *const functions[7]={crypto_suite_0_functions,crypto_suite_1_functions,crypto_suite_2_functions,crypto_suite_3_functions,crypto_suite_4_functions,crypto_suite_5_functions,crypto_suite_6_functions};
static const char *const digests[7]={crypto_suite_0_source_sha256,crypto_suite_1_source_sha256,crypto_suite_2_source_sha256,crypto_suite_3_source_sha256,crypto_suite_4_source_sha256,crypto_suite_5_source_sha256,crypto_suite_6_source_sha256};
int main(int argc,char **argv) {
    CHECK(argc==2);FILE *report=fopen(argv[1],"wb");CHECK(report);
    CHECK(fputs("{\"schema\":1,\"suites\":[",report)>=0);
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    uint32_t passed_total=0;
    for(unsigned s=0;s<7;++s) {
        const CryptoSuite *suite=crypto_suites+s;const XrXirProgramSpec *spec=programs[s];
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,spec,&program)==XR_XIR_OK);
        bool passed[14]={false};
        for(uint32_t i=0;i<suite->count;++i) {
            uint32_t f=functions[s][i];CHECK(f<spec->entry_count);
            const XrXirFunctionIdentity *identity=spec->declarations->functions+f;
            CHECK(identity->test_role==XR_XIR_TEST_ROLE_TEST && !identity->test_timeout_seconds);
            CHECK(identity->module==spec->declarations->root_module);
            passed[i]=crypto_suite_run(program,f,"native",suite->names[i]);if(passed[i])++passed_total;
        }
        xr_xir_compile_program_drop(program);
        crypto_suite_report(report,suite,digests[s],functions[s],passed,true,s);
    }
    xr_compile_resources_release(context.resources);
    CHECK(fprintf(report,"],\"discovered\":67,\"executed\":67,\"passed\":%u,\"skipped\":0,\"failed\":%u}\n",
        passed_total,67-passed_total)>0 && !fclose(report));
    printf("crypto catalog: %u PASS, 0 SKIP, %u FAIL\n",passed_total,67-passed_total);CHECK(passed_total==67);return 0;
}
