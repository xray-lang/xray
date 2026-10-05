/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_conditional_cases.h - Declaration-local method requirements
 */
#ifndef XIR_SOURCE_CONDITIONAL_CASES_H
#define XIR_SOURCE_CONDITIONAL_CASES_H
static void source_conditional_definitions(const XrXirSourceRequest *request,const char *path) {
    AdmissionCase case_owner={0};
    const char *sources[] = {
        "struct C<T>{value:T;checked()->T where T:Sendable{return this.value}}\n",
        "fn outer(callback:fn()->i64=fn()->i64{struct C<T>{checked()->i64 where T:Sendable{return 7}};return 7}){}\n",
        "struct C<T>{value:T;checked()->T where T:Sendable{return this.value}}\nfn outer(callback:fn()->i64=fn()->i64{return 7}){}\n"
    };
    for (size_t i=0;i<sizeof(sources)/sizeof(sources[0]);++i) {
        write_source(path,sources[i]);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        admission_case_begin(request,&case_owner);
        XrXirStatus status=xr_xir_compile_source_check(&case_owner.request,&result,&diagnostic,NULL);
        if(i==1){
            CHECK(status==XR_XIR_BAD_STRUCTURE && !result.checked && !result.snapshot);
            CHECK(diagnostic.status==status && strstr(diagnostic.message,"nested declarations are not admitted"));
            xr_xir_compile_source_result_free(&result);
            continue;
        }
        if(status!=XR_XIR_OK)fprintf(stderr,"conditional definition %zu: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view && view->complete);
        bool found=false;
        for(uint32_t d=0;d<view->declaration_count;++d){
            const XrXirSourceDeclaration *decl=&view->declarations[d];
            if(!strcmp(decl->name,"checked")){
                CHECK(decl->generic_parameter_count==1 && decl->generic_parent_count==1);
                CHECK(decl->generic_constraints && decl->generic_constraints[0].markers==XR_XIR_CONSTRAINT_SENDABLE);found=true;
            }
        }
        CHECK(found);
        XrXirArtifact *closed=NULL;CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL)==XR_XIR_OK && closed);
        xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&result);
    }
    admission_case_end(&case_owner);
}
static void source_conditional_cases(XrXirSourceRequest *request, const char *path) {
    AdmissionCase case_owner={0};
    source_conditional_definitions(request,path);
    const char *prefix = "fn bound<T:Sendable>(x:T)->T{return x}\n"
        "struct Box<T>{value:T;checked()->T where T:Sendable{return bound<T>(this.value)}}\n";
    const struct { const char *source; bool valid; bool box; } cases[] = {
        {"print(Box<i64>{value:7}.checked())\n", true, true},
        {"fn seven()->i64{return 7}\nconst b=Box<fn()->i64>{value:seven}\n", true, true},
        {"fn seven()->i64{return 7}\nconst b=Box<fn()->i64>{value:seven};print(b.checked()())\n", false, true},
        {"fn use<T>(box:Box<T>)->T{return box.checked()}\n", false, true},
        {"fn use<T:Sendable>(box:Box<T>)->T{return box.checked()}\nprint(use<i64>(Box<i64>{value:7}))\n", true, true},
        {"fn use<T>(box:Box<T>)->fn()->T{return box.checked}\n", false, true},
        {"fn use<T:Sendable>(box:Box<T>)->fn()->T{return box.checked}\n", true, true},
        {"struct C<T>{static convert<U>(x:T,y:U)->U where T:Sendable,U:Sendable{const z=bound<T>(x);return bound<U>(y)}}\nprint(C<i64>.convert<string>(7,\"yes\"))\n", true, true},
        {"struct C<T>{value:T;checked()->T where Missing:Sendable{return this.value}}\n", false, false},
        {"struct C<T>{value:T;checked()->T where T:Unknown{return this.value}}\n", false, false},
        {"struct C{checked()->i64 where T:Sendable{return 7}}\n", false, false},
        {"struct C<T>{value:T;constructor(v:T) where T:Sendable{this.value=v}}\n", false, false},
        {"fn duplicate<T:Sendable & Sendable>(x:T)->T where T:Sendable{return x}\nprint(duplicate<i64>(7))\n", true, false},
        {"enum Fault{Bad}\nfn duplicate<T:Error & Error>(x:T)->i64 where T:Error{throw x}\n", true, false},
        {"struct C<T:Sendable>{value:T;checked()->T where T:Sendable,T:Sendable{return this.value}}\nprint(C<i64>{value:7}.checked())\n", true, false},
        {"enum Fault{Bad}\nstruct C<T>{value:T;raise()->i64 where T:Error{throw this.value}}\nfn run()->i64{try{return C<Fault>{value:Fault.Bad}.raise()}catch(e:Fault){return 7}}\nprint(run())\n", true, false},
        {"struct C<T>{value:T;checked()->T where T:Sendable{defer{const x=bound<T>(this.value)};return this.value}}\n", true, true},
        {"struct C<T>{value:T;checked()->fn()->T where T:Sendable{return fn()->T{return bound<T>(this.value)}}}\n", true, true},
        {"struct C<T>{static checked(x:T,f:fn(T)->T=fn(v:T)->T{return bound<T>(v)})->T where T:Sendable{return f(x)}}\nprint(C<i64>.checked(7))\n", true, true},
        {"struct C<T>{static checked(x:T,f:fn(T)->T=fn(v:T)->T{return bound<T>(v)})->T{return f(x)}}\n", false, true},
        {"struct C<T>{checked()->i64 where :Sendable{return 7}}\n", false, false},
        {"struct C<T>{checked()->i64 where T:{return 7}}\n", false, false},
        {"struct C<T>{checked()->i64 where T:Sendable,{return 7}}\n", false, false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char source[2048];
        CHECK(snprintf(source, sizeof(source), "%s%s", cases[i].box ? prefix : "", cases[i].source) > 0);
        write_source(path, source);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        admission_case_begin(request,&case_owner);
        XrXirStatus status = xr_xir_compile_source_check(&case_owner.request, &result, &diagnostic,NULL);
        if ((status == XR_XIR_OK) != cases[i].valid)
            fprintf(stderr, "conditional case %zu: %u at %d: %s\n", i, status, diagnostic.line, diagnostic.message);
        CHECK((status == XR_XIR_OK) == cases[i].valid);
        CHECK((result.checked != NULL) == cases[i].valid);
        if (result.checked) {
            XrXirArtifact *closed = NULL;
            CHECK(xr_xir_compile_specialize(result.checked, &closed, NULL) == XR_XIR_OK);
            xr_xir_compile_artifact_free(closed); xr_xir_compile_artifact_free(result.checked); result.checked = NULL;
            const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(result.snapshot);
            if (cases[i].box) {
                bool found = false;
                for (uint32_t d = 0; d < view->declaration_count; ++d) {
                    const XrXirSourceDeclaration *decl = &view->declarations[d];
                    if (!strcmp(decl->name, "checked")) {
                        CHECK(decl->generic_parameter_count == 1 && decl->generic_parent_count == 1);
                        CHECK(decl->generic_constraints && decl->generic_constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
                        found = true;
                    }
                    if (!strcmp(decl->name, "Box")) {
                        CHECK(decl->generic_parameter_count == 1);
                        CHECK(decl->generic_constraints && decl->generic_constraints[0].markers == 0);
                    }
                    if (!strncmp(decl->name, "$cleanup", 8))
                        CHECK(decl->generic_constraints && decl->generic_constraints[0].markers == XR_XIR_CONSTRAINT_SENDABLE);
                }
                CHECK(found);
            }
        }
        CHECK(status==XR_XIR_OK || (!result.checked && !result.snapshot));
        xr_xir_compile_source_result_free(&result);
    }
    admission_case_end(&case_owner);
}
#endif // XIR_SOURCE_CONDITIONAL_CASES_H
