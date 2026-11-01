/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_crypto_controls.c - Complete ordinary checking and private native boundaries
 *
 * KEY CONCEPT:
 *   A compiler-owned stdlib-authority fixture exercises private ref consumers;
 *   independent project inputs retain import, shadow and typed Error rules.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "base/xsha256.h"
#include "shared/xnative_declaration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"crypto controls %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "crypto_controls_runtime.h"
static uint32_t named(const XrXirModule *module,const char *name) {
    for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) &&
        !memcmp(module->functions[f].name,name,strlen(name)))return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus text_check(const XrXirCompileContext *context,const char *root,const char *text,
    bool crypto,XrXirSourceResult *output) {
    char stdlib[2048],path[2048];CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",root)>0);
    CHECK(snprintf(path,sizeof(path),"%s/tests/unit/xir/fixtures/crypto_context_root.xr",root)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={crypto?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_PROJECT,
        crypto?"crypto":"crypto-context",crypto?NULL:root};
    XrXirSourceRequest request={session,crypto?NULL:path,&authority,context,stdlib,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={crypto?"crypto/crypto.xr":"tests/unit/xir/fixtures/crypto_context_root.xr",text,strlen(text)};
    XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"CONTROL Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);return status;
}
static void native_declarations(const XrXirCompileContext *context,const char *root) {
    static const char source[]="var xs:Array<u8>=[1];__fillRandomBytes(ref xs);"
        "assert(__timingSafeEqualBytes(xs,xs))";
    XrXirSourceResult output={0};
    CHECK(text_check(context,root,source,true,&output)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(output.checked);
    CHECK(module && module->declarations && module->declarations->functions);
    uint32_t fill=0,compare=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        uint32_t id=module->declarations->functions[f].native_callable;
        if(id==XR_NATIVE_CALLABLE_CRYPTO_FILL_RANDOM) {
            CHECK(module->functions[f].parameter_count==1 && module->functions[f].result==XR_XIR_UNIT);
            ++fill;
        } else if(id==XR_NATIVE_CALLABLE_CRYPTO_TIMING_EQUAL) {
            CHECK(module->functions[f].parameter_count==2 && module->functions[f].result==XR_XIR_BOOL);
            ++compare;
        }
    }
    CHECK(fill==1 && compare==1);
    xr_xir_compile_source_result_free(&output);
}
static void ordinary_context(const XrXirCompileContext *context,const char *root) {
    static const char accepted[]="import \"./crypto_context_library\" as lib\n"
        "assertEqual(lib.maybe(),\"x\");assertEqual(try! lib.throwing(),\"x\");"
        "assertEqual(lib.identity(\"z\"),\"z\");var box=try! lib.ContextBox();assertEqual(try! box.read(),7)\n";
    XrXirSourceResult output={0};CHECK(text_check(context,root,accepted,false,&output)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&output);
    static const char *const rejected[]={
        "import \"./crypto_context_library\" as lib;assertEqual(lib.privateMaybe(),\"x\")",
        "import \"./crypto_context_library\" as lib;fn bad(){var lib=\"shadow\";lib.maybe()}",
        "import \"./crypto_context_library\" as lib;assertEqual(lib.identity(null),\"x\")",
        "assertEqual(lib.maybe(),\"x\")",
        "import \"./crypto_context_library\" as lib;enum ContextError{Bad};fn bad()throws(ContextError)->string?{return try lib.throwing()}",
        "fn bad(){var parts=\"x\".split(\"-\")}","var parts=try! \"x\".split()",
        "var parts=try! \"x\".split(\"-\",2)","var parts=try! \"x\".split(false)",
        "enum StringPatternError{EmptyPattern};fn bad()throws(StringPatternError)->Array<string>{return try \"x\".split(\"\")}",
        "var xs:Array<u8>=[1];__fillRandomBytes(ref xs)",
    };
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        CHECK(text_check(context,root,rejected[i],false,&output)!=XR_XIR_OK && !output.checked && !output.snapshot);
        xr_xir_compile_source_result_free(&output);
    }
    static const char *const bad_private[]={
        "const xs:Array<u8>=[1];__fillRandomBytes(ref xs)",
        "var xs:Array<i64>=[1];__fillRandomBytes(ref xs)",
        "var xs:Array<u8>=[1];__fillRandomBytes(xs)",
        "var xs:Array<i64>=[1];var equal=__timingSafeEqualBytes(xs,xs)",
        "var xs:Array<u8>=[1];var equal:i64=__timingSafeEqualBytes(xs,xs)",
    };
    for(unsigned i=0;i<sizeof(bad_private)/sizeof(*bad_private);++i) {
        CHECK(text_check(context,root,bad_private[i],true,&output)!=XR_XIR_OK && !output.checked && !output.snapshot);
        xr_xir_compile_source_result_free(&output);
    }
    const char shadow[]="fn __fillRandomBytes(value:i64)->i64{return value+1};assertEqual(__fillRandomBytes(6),7)";
    CHECK(text_check(context,root,shadow,false,&output)==XR_XIR_OK);xr_xir_compile_source_result_free(&output);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    native_declarations(&context,argv[1]);
    ordinary_context(&context,argv[1]);
    char path[2048],text[16384];CHECK(snprintf(path,sizeof(path),"%s/tests/unit/xir/fixtures/crypto_owner.xr",argv[1])>0);
    FILE *input=fopen(path,"rb");CHECK(input);size_t length=fread(text,1,sizeof(text)-1,input);
    CHECK(length && feof(input) && !ferror(input) && !fclose(input));text[length]=0;
    XrXirSourceResult source={0};CHECK(text_check(&context,argv[1],text,true,&source)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    for(unsigned i=0;i<2;++i) {
        XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=named(module,i?"ctfeCompare":"ctfeFill"),
            .target=target,.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
            .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        memcpy(request.evaluation.checked_identity,lowered->checked_identity,32);XrXirValue result={0};
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&result);
        fprintf(stderr,"CTFE crypto%u status%u\n",i,status);
        CHECK(status==XR_XIR_CTFE_NOT_SUPPORTED && !result.type && !result.reserved && !result.payload);
    }
    uint32_t entry=module->declarations->entry_function,retained=named(module,"retained");
    if(argc==3) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"crypto_controls",16777216,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t crypto_controls_selected[2]={%u,%u};\n",entry,retained)>0 && !fclose(file));
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    crypto_controls_run(program,entry,retained);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);puts("crypto Source permissions, typed split, real refs and retirement PASS");return 0;
}
