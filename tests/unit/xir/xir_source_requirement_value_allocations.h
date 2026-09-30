/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_allocations.h - Bound method producer failure sites
 *
 * KEY CONCEPT:
 *   Every counted source or snapshot allocation fails without partial publication.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_ALLOCATIONS_H
#define XIR_SOURCE_REQUIREMENT_VALUE_ALLOCATIONS_H
#include "xir_requirement_value_fixture.h"
static void source_requirement_value_allocations(XrCompilerSession *session) {
    char directory[]="xir-requirement-values-XXXXXX",absolute[4096],path[8192],manifest[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",absolute)>0);
    CHECK(snprintf(manifest,sizeof(manifest),"%s/xray.toml",absolute)>0);
    const char *programs[]={requirement_value_owned_source,
        ("interface I{map<U:Sendable>(value:U)->U}\n"
        "struct S implements I{map<V:Sendable>(value:V)->V{return value}}\n"
        "fn invoke(f:fn(i64)->i64)->i64{return f(41)}\n"
        "fn bind<T:I>(receiver:T)->i64{return invoke(receiver.map<i64>)}\n"
        "export fn answer()->i64{return bind<S>(S{})}\n")};
    const char *contracts[]={"",
        ("[declarations]\nversion=1\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nowner=\"I\"\nname=\"map\"\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"invoke\"\nno_suspend_parameters=[\"f\"]\nno_suspend=true\n"
        "[[declarations.function]]\nmodule=\"root.xr\"\nname=\"bind\"\nno_suspend=true\n")};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={session,path,&authority,NULL,XR_SOURCE_STDLIB,NULL};
    for (uint32_t mode=0;mode<2;++mode) {
        FILE *file=fopen(path,"wb"); CHECK(file);
        CHECK(fputs(programs[mode],file)>=0 && fclose(file)==0);
        file=fopen(manifest,"wb"); CHECK(file);
        CHECK(fputs(contracts[mode],file)>=0 && fclose(file)==0);
        size_t sites=0;
        for (size_t site=0;site<=sites;++site) {
            CHECK(!live); attempts=0; fail_at=site ? site-1 : SIZE_MAX;
            XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
            XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
            if (!site) {
                if (status!=XR_XIR_OK) fprintf(stderr,"requirement allocation mode %u: %u %s\n",mode,status,diagnostic.message);
                CHECK(status==XR_XIR_OK && result.checked && result.snapshot); sites=attempts;
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY);
                CHECK(!result.checked && !result.snapshot);
                CHECK(!strcmp(diagnostic.message,"source allocation failed") ||
                    !strcmp(diagnostic.message,"source query snapshot publication failed") ||
                    !strcmp(diagnostic.message,"source region identity allocation failed"));
            }
            xr_xir_source_result_free(&result); CHECK(!live);
        }
        fail_at=SIZE_MAX;
        printf("Bound requirement producer mode %u: %zu OOM sites; no partial publication\n",mode,sites);
    }
    CHECK(xr_test_unlink(manifest)==0 && xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_ALLOCATIONS_H
