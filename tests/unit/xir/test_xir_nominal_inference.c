/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nominal_inference.c - Ordered constructor evidence and cold ownership
 *
 * KEY CONCEPT:
 *   Concrete arguments prove nominal applications without retaining the parser.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_types.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Nominal inference %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "nominal_inference_runtime.h"
static const char declarations[]=
    "export class Box<T>{value:T;init(v:T){this.value=v};get()->T{return this.value}}\n"
    "export class Pair<A,B>{first:A;second:B;init(a:A,b:B){this.first=a;this.second=b}}\n";
static const char positive[]=
    "class Box<T>{value:T;init(v:T){this.value=v};get()->T{return this.value}}\n"
    "class Pair<A,B>{first:A;second:B;init(a:A,b:B){this.first=a;this.second=b}}\n"
    "class Phantom<T>{value:i64;init(v:i64){this.value=v}}\n"
    "class Defaulted<T>{value:T;extra:i64;init(v:T,n:i64=2){this.value=v;this.extra=n}}\n"
    "fn wrap<T>(v:T)->Box<T>{return Box(v)}\n"
    "fn reverse<A,B>(a:A,b:B)->Pair<B,A>{return Pair<B,A>(b,a)}\n"
    "var order=0\nfn left()->i64{order=order*10+1;return 20}\n"
    "fn right()->i64{order=order*10+2;return 22}\n"
    "export fn answer()->i64{order=0;var p:Pair<i64,i64>=Pair(left(),right());"
    "assert(order==12);var a=Box(p.first);var b=Box<i64>(p.second);var same:Box<i64>=a;"
    "assert(same.get()==20&&wrap(\"hello\").get()==\"hello\");"
    "var swapped=reverse<i64,string>(1,\"key\");assert(swapped.first==\"key\"&&swapped.second==1);"
    "var context:Phantom<string>=Phantom(9);assert(context.value==9);"
    "var defaults=Defaulted(40);assert(defaults.value+defaults.extra==42);"
    "return a.get()+b.get()}\n";
static uint64_t held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
static XrXirStatus source_check(const XrXirCompileContext *context,const XrXirLibraryCatalog *catalog,
    const char *name,const char *text,XrXirLinkageKind kind,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_NOMINAL_WORK,name)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_NOMINAL_WORK};
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,.context=context,.libraries=catalog,.linkage_kind=kind};
    XrXirSourceText input={name,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"%s Source%u %d:%d %s\n",name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void refusals(const XrXirCompileContext *context) {
    static const char *const inputs[]={
        "class P<A,B>{a:A;init(a:A){this.a=a}}\nvar p=P(1)\n",
        "class P<T>{a:i64;init(a:i64=1){this.a=a}}\nvar p=P()\n",
        "class P<A,B>{a:A;b:B;init(a:A,b:B){this.a=a;this.b=b}}\nvar p=P<i64>(1,2)\n",
        "class P<T>{a:T;init(a:T,b:T){this.a=a}}\nvar p=P(1,\"bad\")\n",
        "class P<T>{a:T;init(a:T){this.a=a}}\nvar p=P<i64>(\"bad\")\n",
        "class P<T>{a:T;init(a:T){this.a=a}}\nvar p=P(1,2)\n",
        "class P<T>{a:T;init(a:T){this.a=a}}\nvar p:P<string>=P(1)\n",
        "class NonSendable{a:i64;init(){this.a=1}}\nclass P<T:Sendable>{a:T;init(a:T){this.a=a}}\nvar p=P(NonSendable())\n"};
    uint64_t before=held(context);
    for(size_t i=0;i<sizeof(inputs)/sizeof(*inputs);++i) {
        XrXirSourceResult result={0};CHECK(source_check(context,NULL,"rejected.xr",inputs[i],XR_XIR_PROGRAM,&result)!=XR_XIR_OK);
        CHECK(!result.checked&&!result.snapshot);xr_xir_compile_source_result_free(&result);CHECK(held(context)==before);
    }
    XrXirCompileContext limited=*context;limited.limits.instructions=1;XrXirSourceResult result={0};
    CHECK(source_check(&limited,NULL,"limited.xr",positive,XR_XIR_PROGRAM,&result)==XR_XIR_BUDGET);
    CHECK(!result.checked&&!result.snapshot);xr_xir_compile_source_result_free(&result);CHECK(held(context)==before);
}
static XrXirLibraryCatalog *cold_catalog(const XrXirCompileContext *context) {
    uint64_t before=held(context);XrXirSourceResult source={0};
    CHECK(source_check(context,NULL,"library.xr",declarations,XR_XIR_LIBRARY,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_NOMINAL_WORK};
    XrXirLibraryModuleInput module={authority,"library.xr"};XrXirLibraryInput input={packet.bytes,packet.length,{0},&module,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(held(context)>before);return catalog;
}
static void execute(const XrXirCompileContext *context,XrXirSourceResult *source,const char *path) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t entry=module->declarations->entry_function,answer=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"answer",6)) {
        CHECK(answer==UINT32_MAX);answer=f;
    }
    CHECK(answer!=UINT32_MAX);
    if(path) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"nominal_inference",8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(path,"wb");CHECK(file&&fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t nominal_inference_ids[2]={%uu,%uu};\n",entry,answer)>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);
    nominal_inference_run(program,entry,answer);
}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    uint64_t before=held(&context);refusals(&context);XrXirSourceResult source={0};
    CHECK(source_check(&context,NULL,"positive.xr",positive,XR_XIR_PROGRAM,&source)==XR_XIR_OK);
    execute(&context,&source,argc==2?argv[1]:NULL);CHECK(held(&context)==before);
    XrXirLibraryCatalog *catalog=cold_catalog(&context);
    static const char consumer[]="import {Box,Pair} from \"./library\"\nexport fn answer()->i64{"
        "var a=Box(20);var b=Box<i64>(22);var p=Pair(\"age\",25);var typed:Box<i64>=a;"
        "assert(typed.get()==20&&p.first==\"age\"&&p.second==25);return a.get()+b.get()}\n";
    CHECK(source_check(&context,catalog,"consumer.xr",consumer,XR_XIR_PROGRAM,&source)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);execute(&context,&source,NULL);CHECK(held(&context)==before);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes&&!source_fixture_compile_live&&!source_fixture_compile_bytes);
    puts("Nominal constructor inference, exact bounds, ordered values, cold VM and retirement PASS");return 0;
}
