/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_source_view_normalize.c - Actual Source lifetime relocation failures
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "tuple_owner_observer.h"
#include <stdlib.h>

static const char *const source_normalize_inputs[]={
    "fn read(v:Slice<i64>)->i64{return len(v)}",
    "fn read(v:Slice<i64>,flag:bool)->i64{if(flag){return len(v)};return 0}",
    "fn read(v:Slice<i64>)->i64{const c:Slice<i64>=v[0:1];return len(c)}",
    "fn read(v:MutSlice<i64>)->i64{const c:Slice<i64>=v[0:1];return len(c)}",
    "enum Failure{Bad}\nclass Box{value:i64;constructor(v:Slice<i64>){try{this.value=len(v)}catch(e:Failure){try{this.value=0}catch(f:Failure){this.value=1}}}}",
    "class Box{value:i64;constructor(v:Slice<i64>){try{try{this.value=len(v)}catch panic(p:PanicInfo){this.value=0}}catch panic(q:PanicInfo){this.value=1}}}",
    "fn read(v:MutSlice<i64>)->i64{const c:MutSlice<i64>=v[0:1];const n=len(v);return n+len(c)}",
    "fn read(v:MutSlice<i64>)->i64{const c:Slice<i64>=v[0:1];const n=len(v);return n+len(c)}",
    "fn read(v:MutSlice<i64>)->i64{const c:MutSlice<i64>=v[0:1];const n=len(c);return n+len(v)}",
    "fn main()->i64{var xs:Array<i64>=[1,2];const c:MutSlice<i64>=xs[0:1];const n=len(xs);return n+len(c)}",
    "fn main()->i64{var xs:Array<i64>=[1,2];const c:MutSlice<i64>=xs[0:1];const n=len(c);return n+len(xs)}",
    "fn helper(v:MutSlice<i64>)->i64{return len(v)}\nfn read(v:MutSlice<i64>)->i64{const c:Slice<i64>=v[0:1];const n=helper(v);return n+len(c)}",
    "fn read(a:MutSlice<i64>,b:MutSlice<i64>)->i64{return len(a)+len(b)}",
    "fn read(a:Slice<i64>,b:MutSlice<i64>)->i64{return len(a)+len(b)}",
    "fn helper(a:MutSlice<i64>)->i64{return len(a)}\nfn read(a:MutSlice<i64>,b:Slice<i64>)->i64{const n=helper(a);return n+len(b)}",
    "fn sink(v:(MutSlice<i64>,MutSlice<i64>)){}\nfn read(v:(MutSlice<i64>,MutSlice<i64>)){sink(v)}",
    "fn sink(v:(MutSlice<i64>,Slice<i64>)){}\nfn read(v:(MutSlice<i64>,Slice<i64>)){sink(v)}",
    "fn read(a:MutSlice<i64>,b:MutSlice<i64>)->i64{const c:MutSlice<i64>=a[0:1];return len(c)+len(b)}",
    "fn read(xs:ref Array<i64>)->i64{const c:Slice<i64>=xs[0:1];return len(c)}",
    "fn read(xs:ref Array<i64>)->i64{const c:Slice<i64>=xs[0:1];return len(c)}\nfn main()->i64{var xs:Array<i64>=[1,2];return read(ref xs)}",
    "fn read(xs:ref Array<i64>)->i64{const c:Slice<i64>=xs[0:1];return len(c)}\nfn main()->i64{var xs:Array<i64>=[1,2];const f=read;return f(ref xs)}",
    "fn main()->i64{const xs:Array<i64>=[1,2];const c:Slice<i64>=xs[0:1];return len(c)}",
    "var xs:Array<i64>=[1,2]\nfn read()->i64{const c:Slice<i64>=xs[0:1];return len(c)}",
    "fn main()->i64{var xs:Array<i64>=[1,2];defer{const c:Slice<i64>=xs[0:1];const n=len(c)};return 0}",
    "fn read(xs:ref Array<i64>,ys:ref Array<i64>)->i64{const a:MutSlice<i64>=xs[0:1];const b:Slice<i64>=ys[0:1];return len(a)+len(b)}\nfn main()->i64{var xs:Array<i64>=[1,2];var ys:Array<i64>=[3,4];return read(ref xs,ref ys)}",
    "fn read(flag:bool)->i64{var n:i64=0;while(flag){var xs:Array<i64>=[1,2];const c:MutSlice<i64>=xs[0:1];n=len(xs)+len(c)};return n}",
    "fn read(v:MutSlice<i64>,flag:bool)->i64{var n:i64=0;while(flag){var xs:Array<i64>=[1,2];const c:MutSlice<i64>=xs[0:1];n=len(v)+len(c)};return n}",
    "fn read(flag:bool)->i64{var n:i64=0;while(flag){var xs:Array<i64>=[1,2];const c:MutSlice<i64>=xs[0:1];if(flag){n=len(xs)+len(c)}};return n}",
    "fn read(v:MutSlice<i64>,flag:bool)->i64{var n:i64=0;while(flag){var xs:Array<i64>=[1,2];if(flag){const c:MutSlice<i64>=xs[0:1];n=len(v)+len(c)}};return n}",
    "fn read(v:MutSlice<i64>,flag:bool)->i64{var n:i64=0;while(flag){const c:MutSlice<i64>=v[0:1];if(flag){n=len(v)+len(c)}};return n}",
    "fn read(v:MutSlice<i64>,flag:bool)->i64{var n:i64=0;while(flag){const c:Slice<i64>=v[0:1];if(flag){n=len(v)+len(c)}};return n}",
    "fn read(v:MutSlice<i64>,flag:bool)->i64{var n:i64=0;while(flag){const c:MutSlice<i64>=v[0:1];if(flag){n=len(c)}};return n}"
};
static XrXirStatus source_normalize_expected(uint32_t kind) {
    return kind==18?XR_XIR_UNRESOLVED:kind==6 || kind==9 || kind==11 || kind==25 || kind==27 || kind==29?XR_XIR_BAD_VALUE:XR_XIR_BAD_TYPE;
}
static XrXirStatus source_normalize_run(const XrXirCompileContext *context,uint32_t kind) {
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:
        made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if (status==XR_XIR_OK) {
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"source-view-lifetime",NULL};
        XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
        XrXirSourceText input={NULL,source_normalize_inputs[kind],strlen(source_normalize_inputs[kind])};
        XrXirSourceDiagnostic diagnostic={0};
        status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
        CHECK(diagnostic.status==status);
        if (status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY) {
            XrXirStatus expected=source_normalize_expected(kind);
            const char *message=expected==XR_XIR_BAD_TYPE?"constructed XIR failed checking at function":
                "constructed XIR failed checking during view lifetime normalization";
            if (status!=expected || !strstr(diagnostic.message,message))
                fprintf(stderr,"Source lifetime kind=%u status=%u: %s\n",kind,status,diagnostic.message);
            CHECK(status==expected && strstr(diagnostic.message,message));
        }
    }
    CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return status;
}
static void source_normalize_resources(uint32_t kind,bool normal) {
    XrXirStatus expected=source_normalize_expected(kind);
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;size_t start=attempts;
    CHECK(source_normalize_run(&context,kind)==expected);
    size_t points=attempts-start;XrCompileResourceStats measured=stats(&context);
    CHECK(points && points<8192);CHECK(source_normalize_run(&context,kind)==expected);owner_free(&context,baseline);
    if (!normal) for (size_t point=0;point<points;++point) {
        context=owner_new(caps());baseline=stats(&context).live_bytes;fail_at=attempts+point;injected=false;
        XrXirStatus status=source_normalize_run(&context,kind);fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY || !injected)
            fprintf(stderr,"Source lifetime kind=%u failure=%zu/%zu status=%u injected=%u\n",kind,point,points,status,injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && injected);owner_free(&context,baseline);
    }
    if (!normal) for (uint32_t axis=0;axis<3;++axis) for (uint32_t delta=0;delta<3;++delta) {
        XrCompileResourceLimits bound=caps();uint64_t *limit=axis==0?&bound.allocated_bytes:axis==1?&bound.live_bytes:&bound.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+delta-1u;
        context=owner_new(bound);baseline=stats(&context).live_bytes;
        CHECK(source_normalize_run(&context,kind)==(delta?expected:XR_XIR_BUDGET));owner_free(&context,baseline);
    }
    printf("Actual Source lifetime kind=%u: %zu allocation points, owner2/physical0%s PASS\n",kind,points,
        normal?"":"/all actual failures/three axes exact+minus1+plus1");
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");setvbuf(stdout,NULL,_IONBF,0);
    CHECK(argc==2);uint32_t count=(uint32_t)(sizeof(source_normalize_inputs)/sizeof(source_normalize_inputs[0]));
    if (normal) for (uint32_t kind=0;kind<count;++kind) source_normalize_resources(kind,true);
    else {
        char *end=NULL;unsigned long selected=strtoul(argv[1],&end,10);
        CHECK(argv[1][0]>='0' && argv[1][0]<='9' && !*end && selected<count);
        source_normalize_resources((uint32_t)selected,false);
    }
    puts("Actual Source lifetime producer rejects execution and restores all compiler physical allocations");return 0;
}
