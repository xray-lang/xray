#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_legacy_admission_cases.h - Current compiler graph for preserved Atomic responsibilities
 */
#ifndef XIR_ATOMIC_LEGACY_ADMISSION_CASES_H
#define XIR_ATOMIC_LEGACY_ADMISSION_CASES_H
static void legacy_admission_cases(void) {
    unsigned positives=0,constructors=0,rejections=0;
    const XrXirType scalars[]={XR_XIR_I64,XR_XIR_BOOL,XR_XIR_F64};
    for(unsigned mode=0;mode<=7;++mode)for(unsigned scalar=0;scalar<3;++scalar){
        if(scalar==1 && mode>=5 && mode!=7)continue;if(scalar==2 && mode==7)continue;
        for(unsigned mutation=0;mutation<(mode>=5?8u:7u);++mutation){
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            char *name=NULL;CHECK(ordering_factory(&owner.context,&name)==XR_XIR_OK);AtomicLegacyFixture fixture;
            AtomicLegacyCase c={mode,mutation,scalars[scalar]};legacy_fixture(&fixture,name,c);
            XrXirArtifact *out=NULL;XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(&owner.context, &fixture.base.module, &out, &diagnostic);
            XrXirStatus expected=mutation==0?XR_XIR_OK:mutation==2||mutation==5||(mutation==7&&mode==7)?XR_XIR_BAD_STRUCTURE:mutation==4?XR_XIR_BAD_DOMINANCE:XR_XIR_BAD_TYPE;
            if(status!=expected)fprintf(stderr,"legacy m%u scalar%u v%u status%u expected%u f%u b%u i%u\n",mode,scalar,mutation,status,expected,diagnostic.function,diagnostic.block,diagnostic.instruction);
            CHECK(status==expected);if(mutation)CHECK(!out);else CHECK(out);
            xr_xir_compile_artifact_free(out);xr_compile_resources_free(name);library_compile_owner_drop(&owner);
            if(!mutation){++positives;char label[64];CHECK(snprintf(label,sizeof(label),"legacy-m%u-s%u",mode,scalar)>0);library_compile_operation_cases(label,legacy_pipeline,&c);}
            else if(mutation==3||mutation==6)++constructors;else ++rejections;
        }
    }
    CHECK(positives==21 && constructors==42 && rejections==90);
    fprintf(stderr,"legacy153 roles21positive/42retired-construction/90verifier current Check PASS; explicit drop runtime responsibility OPEN\n");
}
#endif
