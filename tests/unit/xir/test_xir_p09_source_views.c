/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_source_views.c - Source annotations before executable view admission
 *
 * KEY CONCEPT:
 *   Real parsing reaches the common verifier; descriptors grant no execution.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_fixture_owner.h"

static void source_view_status(const XrXirCompileContext *context,const char *text,const char *message,XrXirStatus expected) {
    XrCompileResourceStats before={0},after={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-view-annotations",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)};XrXirSourceResult result={0};
    XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    if (message) {
        if (status!=expected || !strstr(diagnostic.message,message))
            fprintf(stderr,"Source view status=%u at %d:%d: %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==expected && diagnostic.status==status);
        CHECK(strstr(diagnostic.message,message) && !result.checked && !result.snapshot);
    } else {
        CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
        const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);bool found=false;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length!=4 || memcmp(function->name,"read",4)) continue;
            CHECK(function->parameter_count==1);
            const XrXirTypeNode *parameter=xr_xir_type_node(module->types,function->parameters[0]);
            CHECK(parameter && parameter->kind==XR_XIR_TYPE_NOMINAL);found=true;
        }
        CHECK(found);
    }
    xr_xir_compile_source_result_free(&result);xr_xir_compile_source_failure_free(&failure);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
}
static void source_view_case(const XrXirCompileContext *context,const char *text,const char *message) {
    source_view_status(context,text,message,XR_XIR_BAD_TYPE);
}
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    static const char *const pending[]={
        "fn echo(v:Slice<i64>)->Slice<i64>{return v}",
        "fn ignore(v:MutSlice<i64>){}",
        "fn ignore(v:(Slice<i64>,i64)){}",
        "fn ignore(v:Slice<i64>?){}",
        "fn empty(v:Slice<i64>)->Slice<i64>?{return null}",
        "fn echo<T>(v:Slice<T>)->Slice<T>{return v}",
        "fn read(v:Slice<i64>,flag:bool)->i64{if(flag){return len(v)};return 0}",
        "fn read(v:Slice<i64>)->i64{const c:Slice<i64>=v[0:1];return len(c)}",
        "fn read(v:MutSlice<i64>)->i64{const c:Slice<i64>=v[0:1];return len(c)}",
        "fn read(v:Slice<i64>)->i64{const a:Slice<i64>=v[0:1];const b:Slice<i64>=v[1:2];return len(a)+len(b)}",
        "class Box{value:i64;init(v:Slice<i64>){this.value=len(v)}}",
        "class Box{value:i64;init(v:Slice<i64>){try{this.value=len(v)}catch panic(p:PanicInfo){this.value=0}}}",
        "class Box{value:i64;init(v:Slice<i64>){try{try{this.value=len(v)}catch panic(p:PanicInfo){this.value=0}}catch panic(q:PanicInfo){this.value=1}}}",
        "enum Failure{Bad}\nclass Box{value:i64;init(v:Slice<i64>){try{this.value=len(v)}catch(e:Failure){this.value=0}}}",
        "enum Failure{Bad}\nclass Box{value:i64;init(v:Slice<i64>){try{this.value=len(v)}catch(e:Failure){try{this.value=0}catch(f:Failure){this.value=1}}}}",
    };
    for (size_t i=0;i<sizeof(pending)/sizeof(pending[0]);++i)
        source_view_case(&owner.context,pending[i],"constructed XIR failed checking at function");
    source_view_case(&owner.context,"fn ignore(v:Slice<Slice<i64>>){}","borrowed view elements cannot contain");
    source_view_case(&owner.context,"fn ignore(v:Slice<()>){}","ordinary storable values");
    source_view_case(&owner.context,"fn ignore(v:Slice<i64,i64>){}","one unshadowed element type");
    source_view_case(&owner.context,"fn Slice(){}\nfn ignore(v:Slice<i64>){}","one unshadowed element type");
    source_view_case(&owner.context,"struct Slice<T>{value:T}\nfn read(v:Slice<i64>)->i64{return v.value}",NULL);
    source_view_case(&owner.context,
        "fn main()->i64{var xs:Array<i64>=[1,2];const v:Slice<i64>=xs[0:1];return 0}",
        "constructed XIR failed checking at function");
    source_view_status(&owner.context,
        "fn main()->i64{var xs:Array<i64>=[1,2];const f=fn()->i64{return len(xs)};const v:Slice<i64>=xs[0:1];return f()}",
        "constructed XIR failed checking before view lifetime normalization",XR_XIR_BAD_VALUE);
    source_view_case(&owner.context,
        "fn make()->Slice<i64>{var xs:Array<i64>=[1,2];return xs[0:1]}",
        "constructed XIR failed checking before view lifetime normalization");
    source_view_case(&owner.context,
        "fn choose(a:Slice<i64>,b:Slice<i64>)->Slice<i64>{return a}",
        "constructed XIR failed checking before view lifetime normalization");
    source_fixture_owner_free(&owner);
    puts("Source view annotations and executable rejection controls PASS; root/path/NLL chain remains open");
    return 0;
}
