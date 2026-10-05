/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_assert_condition.c - Typed intrinsic declaration and owned execution
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
#include "xir_assert_condition_cases.h"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"

static void assert_write(const char *path, const char *source) {
    FILE *file=fopen(path,"wb"); CHECK(file);
    size_t size=strlen(source);
    CHECK(fwrite(source,1,size,file)==size && fclose(file)==0);
}
#include "xir_assert_condition_source_gates.inc.c"
static uint32_t assert_find(const XrXirModule *module, const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false); return UINT32_MAX;
}
static XrXirSourceResult assert_source(const char *directory, const char *path,
    const char *source, bool valid) {
    assert_write(path,source);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK); CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,path,&authority,assert_compile_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult output={0}; XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&output,&diagnostic,NULL);
    xr_compile_session_free(session);
    if ((status==XR_XIR_OK)!=valid) fprintf(stderr,"source status %u %d:%d %s\n%s\n",
        status,diagnostic.line,diagnostic.column,diagnostic.message,source);
    CHECK((status==XR_XIR_OK)==valid && (output.checked!=NULL)==valid && (output.snapshot!=NULL)==valid);
    return output;
}
static void assert_bindings(const XrXirSourceResult *source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source->snapshot);
    CHECK(view && view->complete && view->module_count==2 && module->declarations->module_count==2);
    CHECK(module->defaults && module->defaults->count==3);
    const XrXirDefaultBinding *binding=&module->defaults->records[0];
    CHECK(binding->owner_kind==XR_XIR_DEFAULT_PARAMETER && binding->ordinal==1);
    const XrXirFunction *owner=&module->functions[binding->owner];
    const XrXirFunction *helper=&module->functions[binding->function];
    CHECK(owner->parameter_count==2 && owner->parameters[0]==XR_XIR_BOOL &&
        owner->parameters[1]==XR_XIR_STRING && owner->result==XR_XIR_UNIT);
    CHECK(helper->parameter_count==0 && helper->result==XR_XIR_STRING);
    CHECK(module->declarations->functions[binding->owner].exported &&
        !module->declarations->functions[binding->function].exported);
    CHECK(module->declarations->functions[binding->owner].module==1 &&
        module->declarations->functions[binding->function].module==1);
    CHECK(owner->instructions[0].op==XR_XIR_ASSERT_CONDITION && owner->instructions[0].args[0]==0 &&
        owner->instructions[0].args[1]==1 && owner->instructions[1].op==XR_XIR_RETURN);
    CHECK(helper->instructions[0].op==XR_XIR_CONST_STRING && helper->instructions[1].op==XR_XIR_RETURN);
    CHECK(module->declarations->literals[helper->instructions[0].immediate].length==0);
    CHECK(module->declarations->entry_function+3<module->function_count);
    uint32_t omitted=assert_find(module,"omitted"), defaults=0;
    for (uint32_t i=0;i<module->functions[omitted].instruction_count;++i) {
        const XrXirInstruction *op=&module->functions[omitted].instructions[i];
        if (op->op==XR_XIR_CALL_DEFAULT) {
            CHECK(op->targets[0]==binding->owner && op->targets[1]==1); ++defaults;
        }
        if (op->op==XR_XIR_CONST_STRING)
            CHECK(module->declarations->literals[op->immediate].length!=0);
    }
    CHECK(defaults==1);
    XrFingerprint fingerprint={0}; xr_module_source_fingerprint(xir_core_declaration_source,&fingerprint);
    CHECK(!memcmp(&view->modules[1].fingerprint,&fingerprint,sizeof(fingerprint)));
    uint32_t core=0, parameters=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->native_identity==XR_CORE_BUILTIN_ASSERT) {
            CHECK(decl->kind==XR_XIR_SOURCE_INTRINSIC && decl->range.module==1 && decl->parameter_count==2);
            CHECK(decl->parameters[0].known && decl->parameters[0].type==XR_XIR_BOOL);
            CHECK(decl->parameters[1].known && decl->parameters[1].type==XR_XIR_STRING);
            ++core;
        }
        if (decl->range.module==1 && decl->kind==XR_XIR_SOURCE_PARAMETER) ++parameters;
    }
    CHECK(core==1 && parameters==7);
    XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(source->checked,&effects)==XR_XIR_OK);
    const XrXirFunctionEffects *effect=xr_xir_effects_function(effects,binding->owner);
    CHECK(effect && effect->suspend==XR_XIR_EFFECT_NONE && effect->throws==XR_XIR_EFFECT_NONE);
    xr_xir_compile_effects_free(effects);
}
static XrXirArtifact *assert_lower(XrXirSourceResult *source) {
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(source);
    XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xCC,packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK); xr_xir_compile_artifact_free(read);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK); xr_xir_compile_artifact_free(closed);
    return lowered;
}
#include "xir_assert_condition_packet_gates.inc.c"
#include "xir_assert_condition_authority_gates.inc.c"
#include "xir_assert_condition_pipeline_gates.inc.c"
#include "xir_assert_condition_initialization.inc.c"
#include "xir_assert_condition_lexical_gates.inc.c"
static const char *assert_names[ASSERT_FUNCTIONS]={"omitted","message","code","success","ordered",
    "information","raw","messagePanic","suspended","cleanup"};
static void assert_entries(const XrXirModule *module,uint32_t *functions) {
    for (uint32_t i=0;i<ASSERT_FUNCTIONS;++i) functions[i]=assert_find(module,assert_names[i]);
}
static void assert_vm(XrXirArtifact *lowered) {
    uint32_t functions[ASSERT_FUNCTIONS]; assert_entries(xr_xir_compile_artifact_module(lowered),functions);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    CHECK(!lowered); assert_cases(program,functions);
}
int main(int argc,char **argv) {
    assert_compile_begin();
    CHECK(argc==3);
    char path[2048]; CHECK(snprintf(path,sizeof(path),"%s/root.xr",argv[1])>0);
    const char *source=
        "export fn omitted()->string {try{assert(false)}catch panic(p){return p.message};return \"bad\"}\n"
        "export fn message()->string {try{assert(false,\"a\\n\\r\\t中Z\")}catch panic(p){return p.message};return \"bad\"}\n"
        "export fn code()->i64 {try{assert(false)}catch panic(p){return p.code};return -1}\n"
        "export fn success()->i64 {assert(true,\"success\");return 23}\n"
        "export fn ordered()->i64 {var n=0;const cond=fn()->bool{n=n*10+1;return true};const msg=fn()->string{n=n*10+2;return \"yes\"};assert(cond(),msg());return n}\n"
        "export fn information()->PanicInfo {try{assert(false,\"held\")}catch panic(p){return p};return information()}\n"
        "export fn raw(cond:bool,msg:string)->PanicInfo {try{assert(cond,msg)}catch panic(p){return p};return information()}\n"
        "fn badMessage()->string {const n=1/0;return \"bad\"}\n"
        "export fn messagePanic()->i64 {try{assert(true,badMessage())}catch panic(p){return p.code};return -1}\n"
        "fn paused()->string {Coro.yield();return \"paused\"}\n"
        "export fn suspended()->string {try{assert(false,paused())}catch panic(p){Coro.yield();return p.message};return \"bad\"}\n"
        "export fn cleanup()->i64 {var n=0;try{defer {n=7};assert(false,\"x\")}catch panic(p){return n};return -1}\n";
    XrXirSourceResult result=assert_source(argv[1],path,source,true);
    assert_bindings(&result);
    assert_core_packet(&result);
    assert_permissions(&result);
    assert_pipeline_oom(result.checked);
    XrXirArtifact *lowered=assert_lower(&result);
    XrXirCSource generated={0}; CHECK(xr_xir_compile_emit_c(lowered,"assert_condition",4194304,&generated)==XR_XIR_OK);
    CHECK(!strstr(generated.text,"({"));
    FILE *file=fopen(argv[2],"wb");CHECK(file);
    CHECK(fwrite(generated.text,1,generated.length,file)==generated.length);
    uint32_t functions[ASSERT_FUNCTIONS];assert_entries(xr_xir_compile_artifact_module(lowered),functions);
    CHECK(fprintf(file,"\nconst uint32_t assert_condition_functions[%u]={",ASSERT_FUNCTIONS)>0);
    for (uint32_t i=0;i<ASSERT_FUNCTIONS;++i) CHECK(fprintf(file,"%s%uu",i ? "," : "",functions[i])>0);
    CHECK(fputs("};\n",file)>=0 && fclose(file)==0);
    xr_xir_compile_c_source_free(&generated); assert_vm(lowered);
    const char *invalid[]={"assert()","assert(1)","assert(true,1)","assert(true,null)","assert(true,\"x\",\"y\")",
        "assert<i64>(true)","const f=assert","check(true)",
        "var flag=true;assert(ref flag)","var flag=true;assert(move flag)",
        "fn unused(){assert(1)}", "fn wrong<T>(value:T){assert(value)}", "assertEqual(1,true)",
        "assertPanics(fn(value:i64){})"};
    for (uint32_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        result=assert_source(argv[1],path,invalid[i],false);xr_xir_compile_source_result_free(&result);
    }
    result=assert_source(argv[1],path,"fn assert(cond:bool,msg:string=\"\"){}\nassert(false)\n",true);
    CHECK(xr_xir_compile_artifact_module(result.checked)->declarations->module_count==1);
    CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->module_count==1);
    xr_xir_compile_source_result_free(&result);
    assert_lexical(argv[1],path);
    assert_source_oom(argv[1],path);
    assert_ordinary_memory_origin();
    assert_initialization(argv[1],path);
    CHECK(runtime_live==0 && runtime_bytes==0 && !assert_compile_extra_blocks() && !assert_compile_extra_bytes());
    puts("Typed assert Source/default owner, owned packet, VM, lexical shadow and invalid calls PASS");
    assert_compile_end();return 0;
}
