/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_equal.c - Predicate proofs and a single typed equality owner
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "base/xmalloc.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)

#include "xir_assert_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
static void panics_write(const char *path,const char *source) {
    FILE *file=fopen(path,"wb");CHECK(file);
    size_t length=strlen(source);CHECK(fwrite(source,1,length,file)==length && fclose(file)==0);
}
static XrXirSourceResult panics_source(const char *directory,const char *path,const char *source,bool valid) {
    panics_write(path,source);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,path,&authority,assert_compile_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    xr_compile_session_free(session);
    if ((status==XR_XIR_OK)!=valid) fprintf(stderr,"check=%u %d:%d %s\n%s\n",status,diagnostic.line,diagnostic.column,diagnostic.message,source);
    CHECK((status==XR_XIR_OK)==valid && (result.checked!=NULL)==valid && (result.snapshot!=NULL)==valid);
    return result;
}
static uint32_t panics_find(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirArtifact *panics_lower(XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_specialize(read,&closed,&diagnostic);
    if (status!=XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(read);
        fprintf(stderr,"panics specialize=%u f=%u b=%u i=%u source-name=%.*s\n",status,
            diagnostic.function,diagnostic.block,diagnostic.instruction,
            diagnostic.function<module->function_count ? (int)module->functions[diagnostic.function].name_length : 0,
            diagnostic.function<module->function_count ? module->functions[diagnostic.function].name : "");
    }
    CHECK(status==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return lowered;
}
#include "xir_assert_equal_packet.inc.c"
#include "xir_assert_equal_source_oom.inc.c"
#include "xir_assert_equal_pipeline_oom.inc.c"
#include "xir_assert_equal_inputs.h"
static void equal_rejections(const char *directory,const char *path) {
    const char *sources[]={
        "fn unused<T>(a:T,b:T)->bool{return a==b}",
        "fn unused<T:Sendable>(a:T,b:T){assertEqual(a,b)}",
        "fn unused<T>(a:Array<T>,b:Array<T>)->bool{return a!=b}",
        "fn unused<T:Equal<i64>>(a:T,b:T)->bool{return a==b}",
        "interface Equal{};fn unused<T:Equal>(a:T,b:T)->bool{return a==b}",
        "interface Equal{};fn unused<T:Equal>(a:T,b:T){assertEqual(a,b)}",
        "struct Equal{value:i64};fn unused<T:Equal>(a:T,b:T)->bool{return a==b}",
        "fn unused<Equal,T:Equal>(a:T,b:T)->bool{return a==b}",
        "assertEqual((),())",
        "assertEqual(fn(){},fn(){})",
        "var a:i64=7;var b:u8=7;assertEqual(a,b)",
        "var a:i64=7;var b:u8=7;assert(a==b)",
        "var a:f32=1.0;var b:f64=1.0;assert(a!=b)",
        "assertEqual(1,1,null)",
        "struct S{value:i64};assertEqual(S{value:1},S{value:1})",
        "var a:i64?=1;var b:u8=1;assertEqual(a,b)"
    };
    for (uint32_t i=0;i<sizeof(sources)/sizeof(*sources);++i) {
        XrXirSourceResult result=panics_source(directory,path,sources[i],false);
        xr_xir_compile_source_result_free(&result);
        CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    }
    const char *ordinary="interface Equal{};fn ordinary<T:Equal>(a:T){}";
    XrXirSourceResult result=panics_source(directory,path,ordinary,true);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    uint32_t owner=panics_find(module,"ordinary");
    CHECK(!module->generics[owner].constraints[0].markers && module->generics[owner].constraints[0].interface_count==1);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(view->module_count==1);
    for (uint32_t d=0;d<view->declaration_count;++d) CHECK(!view->declarations[d].native_identity);
    xr_xir_compile_source_result_free(&result);
    result=panics_source(directory,path,
        "fn assertEqual<T>(a:T,b:T,msg:string=\"\"){};assertEqual(fn(){},fn(){})",true);
    module=xr_xir_compile_artifact_module(result.checked);view=xr_xir_compile_source_snapshot_view(result.snapshot);
    CHECK(module->declarations->module_count==1 && view->module_count==1);
    owner=panics_find(module,"assertEqual");CHECK(!module->generics[owner].constraints[0].markers);
    for (uint32_t f=0;f<module->function_count;++f) for (uint32_t i=0;i<module->functions[f].instruction_count;++i)
        CHECK(module->functions[f].instructions[i].op!=XR_XIR_EQUAL &&
            module->functions[f].instructions[i].op!=XR_XIR_ASSERT_CONDITION);
    for (uint32_t d=0;d<view->declaration_count;++d) CHECK(!view->declarations[d].native_identity);
    xr_xir_compile_source_result_free(&result);
}
static void equal_default_owner(const XrXirSourceResult *result) {
    const XrXirModule *module=xr_xir_compile_artifact_module(result->checked);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result->snapshot);
    CHECK(module->defaults && module->defaults->count==3 && view->complete &&
        module->declarations->module_count==2 && view->module_count==4);
    CHECK(!strcmp(view->modules[1].identity,"xray-native:prelude/Array") &&
        !strcmp(view->modules[2].identity,"memory-module-v1:id=23:xray-core-assertions-v1") &&
        !strcmp(view->modules[3].identity,"xray-core:prelude"));
    uint32_t owner=panics_find(module,"assertEqual");
    const XrXirGeneric *generic=&module->generics[owner];
    CHECK(generic->parameter_count==1 && !generic->parameter_kinds &&
        generic->constraints[0].markers==XR_XIR_CONSTRAINT_EQUAL && !generic->constraints[0].interface_count);
    const XrXirDefaultBinding *binding=&module->defaults->records[2];
    CHECK(binding->owner==owner && binding->ordinal==2 && binding->owner_kind==XR_XIR_DEFAULT_PARAMETER);
    const XrXirFunction *helper=&module->functions[binding->function];
    CHECK(!helper->parameter_count && !helper->parameters && helper->result==XR_XIR_STRING);
    CHECK(!module->declarations->functions[binding->function].exported &&
        module->generics[binding->function].constraints[0].markers==XR_XIR_CONSTRAINT_EQUAL);
    CHECK(module->functions[owner].instructions[0].op==XR_XIR_EQUAL &&
        module->functions[owner].instructions[0].immediate==0 &&
        module->functions[owner].instructions[1].op==XR_XIR_ASSERT_CONDITION);
    CHECK(helper->instructions[0].op==XR_XIR_CONST_STRING &&
        module->declarations->literals[helper->instructions[0].immediate].length==0);
    uint32_t omitted=panics_find(module,"contextual"),defaults=0;
    for (uint32_t i=0;i<module->functions[omitted].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[omitted].instructions[i];
        if (op->op==XR_XIR_CALL_DEFAULT && op->targets[0]==owner) {CHECK(op->targets[1]==2);++defaults;}
    }
    CHECK(defaults==2);
    uint32_t found=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->native_identity==XR_CORE_BUILTIN_ASSERT_EQUAL) {
            CHECK(decl->kind==XR_XIR_SOURCE_INTRINSIC && decl->generic_parameter_count==1 &&
                !decl->type_parameter_kinds && decl->generic_constraints[0].markers==XR_XIR_CONSTRAINT_EQUAL);
            ++found;
        }
    }
    CHECK(found==1);
}
static void equal_run(XrXirArtifact *lowered,const char *generated_path) {
    const char *names[]={"bools","contextual","arrays","nan","zero","nulString","emptyMessage",
        "ownedMessage","once","forwarded","whereProof","arrayProof","operatorProof"};
    const int64_t expected[]={1,7,3,5,6,7,0,3,123,11,12,13,14};
    uint32_t entries[13];const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    for (uint32_t i=0;i<13;++i) entries[i]=panics_find(module,names[i]);
    XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"equal_checked",16777216,&output)==XR_XIR_OK);
    CHECK(strstr(output.text,"xr_xir_value_equal") && !strstr(output.text,"({"));
    FILE *generated=fopen(generated_path,"wb");CHECK(generated);
    CHECK(fwrite(output.text,1,output.length,generated)==output.length);
    CHECK(fputs("\nconst uint32_t equal_checked_functions[13]={",generated)>=0);
    for (uint32_t i=0;i<13;++i) CHECK(fprintf(generated,"%s%uu",i ? "," : "",entries[i])>0);
    CHECK(fputs("};\nconst uint32_t equal_checked_inputs[2]={",generated)>=0);
    uint32_t relation=panics_find(module,"rawEqual"),assertion=panics_find(module,"rawMessage");
    CHECK(fprintf(generated,"%uu,%uu};\n",relation,assertion)>0 && fclose(generated)==0);xr_xir_compile_c_source_free(&output);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    equal_instance_oom(program,entries[2]);
    XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<13;++i) {
        CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance, UINT64_MAX);
        if (polled.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"equal %s status=%u\n",names[i],polled.outcome.status);
        CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (value.payload!=expected[i]) fprintf(stderr,"equal %s got=%lld expected=%lld\n",names[i],(long long)value.payload,(long long)expected[i]);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[i]);xr_xir_value_drop(&value);
    }
    XrXirValue held[2]={{0}};equal_owned_inputs(instance,relation,assertion,held);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);equal_held_messages(held);
}
static void equal_existing_fixture(const char *directory,const char *path,const char *fixture,const char *generated_path) {
    FILE *file=fopen(fixture,"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
    CHECK(length>0 && length<1048576 && !fseek(file,0,SEEK_SET));
    char *source=xr_malloc((size_t)length+1);CHECK(source);
    CHECK(fread(source,1,(size_t)length,file)==(size_t)length && !fclose(file));source[length]=0;
    XrXirSourceResult result=panics_source(directory,path,source,true);xr_free(source);
    XrXirArtifact *lowered=panics_lower(&result);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t entry=module->declarations->entry_function;CHECK(entry<module->function_count);
    XrXirCSource generated={0};
    CHECK(xr_xir_compile_emit_c(lowered,"nullable_checked",16777216,&generated)==XR_XIR_OK);
    CHECK(!strstr(generated.text,"({"));FILE *output=fopen(generated_path,"wb");CHECK(output);
    CHECK(fwrite(generated.text,1,generated.length,output)==generated.length && !fclose(output));
    xr_xir_compile_c_source_free(&generated);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    puts("Existing exact fixture bytes through Source/Checked/packet/VM PASS; public CLI consumer remains separate");
}
int main(int argc,char **argv) {
    assert_compile_begin();
    CHECK(argc==3 || argc==4 || argc==5);char path[2048];CHECK(snprintf(path,sizeof(path),"%s/equal.xr",argv[1])>0);
    if (argc==5) {
        CHECK(!strcmp(argv[3],"fixture"));const char *name=argv[2];
        const char *slash=strrchr(name,'/'),*backslash=strrchr(name,'\\');
        if (slash) name=slash+1;
        if (backslash && backslash>=name) name=backslash+1;
        int length=snprintf(path,sizeof(path),"%s/fixture-%s.xr",argv[1],name);
        CHECK(length>0 && (size_t)length<sizeof(path));
        equal_existing_fixture(argv[1],path,argv[4],argv[2]);assert_compile_end();return 0;
    }
    if (argc==4) {
        if (!strcmp(argv[3],"nullable-oom")) {
            equal_source_oom(argv[1],path,
                "fn same<T:Equal>(a:T,b:T)->bool{return a==b};export fn run(){var a:Array<string?>=[null,\"a\"];assert(same(a,a));var n:i64?=7;assertEqual(7,n)}\n");
            equal_pipeline_oom(argv[1],path,
                "export fn run(){var a:Array<string?>=[null,\"owned\"];var n:Array<string?>?=a;assertPanics(fn(){assertEqual(n,a)},\"normal return\")}\n");
        } else {
            CHECK(!strcmp(argv[3],"oom"));
            equal_source_oom(argv[1],path,"fn same<T:Equal>(a:Array<T>,b:Array<T>)->bool{return a==b};export fn run(){assertEqual([1],[1]);assert(same([1],[1]))}\n");
            equal_pipeline_oom(argv[1],path,"export fn run(){assertPanics(fn(){assertEqual([[1]],[[1]])},\"normal return\")}\n");
        }
        puts("Equal Source/default/proof/pipeline/runtime OOM and exact-budget physical refunds PASS");assert_compile_end();return 0;
    }
    equal_packet_gates();
    equal_rejections(argv[1],path);
    const char *source=
        "fn same<T:Equal>(a:T,b:T)->bool{return a==b}\n"
        "fn different<T>(a:T,b:T)->bool where T:Equal{return a!=b}\n"
        "fn relay<T:Equal>(a:T,b:T)->bool{return same(a,b)}\n"
        "fn arraySame<T:Equal>(a:Array<T>,b:Array<T>)->bool{return a==b}\n"
        "export fn bools()->i64{assertEqual(true,true);assert(true!=false);return 1}\n"
        "export fn contextual()->i64{const v:u8=7;assertEqual(v,7);assertEqual(7,v);assert(v==7);assert(7==v);assert(v!=8);assert(8!=v);return 7}\n"
        "export fn arrays()->i64{assertEqual([[1,2],[3]],[[1,2],[3]]);assertPanics(fn(){assertEqual([1],[2])});return 3}\n"
        "export fn nan()->i64{var z=0.0;const n=z/z;const a=[n];const b=a;assert(a!=b);assertPanics(fn(){assertEqual(a,b)});return 5}\n"
        "export fn zero()->i64{assertEqual(-0.0,0.0);assertEqual([-0.0],[0.0]);return 6}\n"
        "export fn nulString()->i64{assertEqual(\"a中b\",\"a中b\");assert(\"a中b\"!=\"a中c\");return 7}\n"
        "export fn emptyMessage()->i64{try{assertEqual(1,2)}catch panic(p){return len(p.message)};return -1}\n"
        "export fn ownedMessage()->i64{try{assertEqual(1,2,\"abc\")}catch panic(p){return len(p.message)};return -1}\n"
        "export fn once()->i64{var s=0;const a=fn()->i64{s=s*10+1;return 7};const b=fn()->i64{s=s*10+2;return 7};const m=fn()->string{s=s*10+3;return \"m\"};assertEqual(a(),b(),m());return s}\n"
        "export fn forwarded()->i64{assert(relay([1,2],[1,2]));return 11}\n"
        "export fn whereProof()->i64{assert(different(1,2));return 12}\n"
        "export fn arrayProof()->i64{assert(arraySame([\"a\"],[\"a\"]));return 13}\n"
        "export fn operatorProof()->i64{assert(same(true,true));assert(!same(1.0,2.0));return 14}\n"
        "export fn rawEqual(a:string,b:string)->bool{return a==b}\n"
        "export fn rawMessage(msg:string)->PanicInfo{try{assertEqual(1,2,msg)}catch panic(p){return p};return rawMessage(\"fallback\")}\n";
    XrXirSourceResult result=panics_source(argv[1],path,source,true);equal_default_owner(&result);
    XrXirArtifact *lowered=panics_lower(&result);equal_run(lowered,argv[2]);
    CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    puts("Equal declaration/default/proof negatives and 13 typed Source/packet/VM outcomes PASS");assert_compile_end();return 0;
}
