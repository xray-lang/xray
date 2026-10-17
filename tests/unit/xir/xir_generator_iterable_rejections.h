/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_iterable_rejections.h - Implicit iteration retains declaration authority
 */
#ifndef XIR_GENERATOR_ITERABLE_REJECTIONS_H
#define XIR_GENERATOR_ITERABLE_REJECTIONS_H
static void iterable_rejections(void) {
    static const struct {const char *text,*message;} cases[]={
        {"struct S{}\nfn main(){for(v in S()){}}\n","requires an iterator method"},
        {"struct S{static iterator()->Iterator<i64>{return values()}}\nfn values()->Iterator<i64>{yield 1}\nfn main(){for(v in S()){}}\n","static method requires type-qualified"},
        {"struct S{private iterator()->Iterator<i64>{return values()}}\nfn values()->Iterator<i64>{yield 1}\nfn main(){for(v in S()){}}\n","method requires its declaration owner"},
        {"struct S{iterator()->Array<i64>{return [1]}}\nfn main(){for(v in S()){}}\n","iterator method must return"},
        {"struct S{iterator(v:i64)->Iterator<i64>{return values(v)}}\nfn values(v:i64)->Iterator<i64>{yield v}\nfn main(){for(v in S()){}}\n","argument"},
        {"var state=0\nfn values()->Iterator<i64>{yield 1}\nstruct S{iterator()->Iterator<i64>{state=state+1\nreturn values()}}\nfn worker(){for(v in S()){}}\nfn main(){const task=go worker()}\n",""},
        {"struct S{value:i64 ref iterator()->Iterator<i64>{yield this.value}}\nfn main(){}\n","generator cannot retain an external ref parameter"}
    };
    for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"iterator-authority",NULL};
        XrXirSourceRequest request={session,NULL,&authority,&owner.context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,
            &(XrXirSourceText){NULL,cases[n].text,strlen(cases[n].text)},&source,&diagnostic,NULL);
        if(status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.message,cases[n].message))
            fprintf(stderr,"Iterable negative%u status%u: %s\n",n,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !source.checked && strstr(diagnostic.message,cases[n].message));
        xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);source_fixture_owner_free(&owner);
    }
}
#endif
