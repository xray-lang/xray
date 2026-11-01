/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_move_source.c - Real consuming declarations and owned parameter bodies
 *
 * KEY CONCEPT:
 *   Source declares the consuming ordinal while its physical parameter stays
 *   the original value type. Rebinding uses a distinct owned local Cell.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "base/xsha256.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"move Source %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "move_runtime.h"
#include "xir_construction_fixture.h"
static void move_test_previous(const XrXirCompileContext *context) {
    FILE *file=fopen(XR_MOVE_PREVIOUS_PACKET,"rb");CHECK(file);uint8_t bytes[1421];
    CHECK(fread(bytes,1,sizeof(bytes),file)==sizeof(bytes)&&fgetc(file)==EOF&&!fclose(file));
    static const uint8_t expected[32]={0x68,0xbe,0x0a,0x02,0x9a,0x8d,0x65,0x63,0x44,0xe5,0xf9,0xa3,0x98,0x9c,0x42,0x98,
        0xcb,0x19,0x41,0x13,0x75,0x7f,0x2a,0x58,0x15,0x49,0x18,0xd9,0x87,0xc6,0xec,0x21};
    uint8_t digest[32];xr_sha256(bytes,sizeof(bytes),digest);CHECK(!memcmp(digest,expected,sizeof(digest)));
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=NULL;CHECK(xr_xir_compile_checked_read(context,bytes,sizeof(bytes),&artifact,NULL)!=XR_XIR_OK&&!artifact);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
}
static void move_test_panic_graph(const XrXirCompileContext *context) {
    const XrXirType parameters[]={XR_XIR_I64};const uint32_t consumed[]={0},operands[]={0};
    XrXirInstruction caller[]={
        {.op=XR_XIR_JUMP,.targets={1}},
        {.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=1},
        {.op=XR_XIR_RETURN,.args={2}},
        {.op=XR_XIR_PANIC_CATCH},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_RETURN,.args={5}},
    };
    const XrXirInstruction callee[]={{.op=XR_XIR_RETURN,.args={0}}};
    const XrXirBlock blocks[]={{.count=1},{.first=1,.count=2,.panic=2},{.first=3,.count=3}},leaf[]={{.count=1}};
    XrXirFunction functions[]={
        {.name="caller",.name_length=6,.parameters=parameters,.parameter_count=1,.result=XR_XIR_I64,
         .instructions=caller,.instruction_count=6,.blocks=blocks,.block_count=3,.operands=operands,.operand_count=1,
         .error_type=XR_XIR_NEVER,.consume_parameter_count=1,.consume_parameters=consumed},
        {.name="callee",.name_length=6,.parameters=parameters,.parameter_count=1,.result=XR_XIR_I64,
         .instructions=callee,.instruction_count=1,.blocks=leaf,.block_count=1,.error_type=XR_XIR_NEVER,
         .consume_parameter_count=1,.consume_parameters=consumed},
    };
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=2,.linkage_kind=XR_XIR_PROGRAM};
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *artifact=NULL;XrXirDiagnostic issue={0};
    XrXirStatus status=xir_fixture_check(context,&module,&artifact,&issue);
    fprintf(stderr,"MOVE panic valid%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(artifact);artifact=NULL;
    caller[5].args[0]=0;status=xir_fixture_check(context,&module,&artifact,&issue);
    fprintf(stderr,"MOVE panic refusal%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);
    CHECK(status==XR_XIR_BAD_VALUE&&!artifact&&issue.function==0&&issue.block==2&&issue.instruction==5);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
}
static void move_test_refusals(const XrXirCompileContext *context) {
    static const char *const bodies[]={
        "fn bad(xs:Array<i64>)->i64{return eat(move xs)}",
        "fn bad(xs:ref Array<i64>)->i64{return eat(move xs)}",
        "var xs=[1];fn bad()->i64{return eat(move xs)}",
        "fn bad()->i64{var xs=[1];eat(move xs);return xs[0]}",
        "fn bad()->i64{const xs=[1];eat(move xs);return eat(move xs)}",
        "fn bad(flag:bool)->i64{var xs=[1];if(flag){eat(move xs)};return xs[0]}",
        "fn bad()->i64{var xs=[1];var n=0;while(n<2){eat(move xs);n+=1};return n}",
        "fn bad()->i64{var xs=[1];fn read()->i64{return xs[0]};eat(move xs);return read()}",
        "fn bad()->i64{var xs=[1];const read=fn()->i64{return xs[0]};eat(move xs);return read()}",
        "fn scalar(x:move i64)->i64{return x};fn bad()->i64{var xs=[1];return scalar(move xs[0])}",
        "const action:fn(Array<i64>)->i64=eat",
        "fn bytes(xs:move Array<u8>)->i64{return len(xs)};fn bad()->i64{var xs:Array<u8>=[1];var view:Slice<u8>=xs[:];bytes(move xs);return view[0] as i64}",
        "fn bad()->i64{var xs=[1];defer{print(xs[0])};return eat(move xs)}",
        "fn bad()->i64{var saved:fn()->i64=fn()->i64{return 0};var n=0;while(n<2){var xs=[42];if(n==1){return saved()};saved=fn()->i64{return xs[0]};eat(move xs);n+=1};return 0}",
        "fn bad()->i64{var src=([1],[2]);var(a,b)=move src;return a[0]+b[0]}",
    };
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(size_t i=0;i<sizeof(bodies)/sizeof(*bodies);++i) {
        char text[2048];int count=snprintf(text,sizeof(text),"fn eat(xs:move Array<i64>)->i64{return xs[0]};%s\n",bodies[i]);
        CHECK(count>0&&(size_t)count<sizeof(text));XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"move-refusal",NULL};
        XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
        XrXirSourceText input={.text=text,.length=(size_t)count};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
        fprintf(stderr,"MOVE refusal%zu status%u %d:%d %s\n",i,status,diagnostic.line,diagnostic.column,diagnostic.message?diagnostic.message:"");
        CHECK(status!=XR_XIR_OK&&!result.checked&&!result.snapshot);
        if(i+1==sizeof(bodies)/sizeof(*bodies))CHECK(status==XR_XIR_UNSUPPORTED&&diagnostic.message&&
            !strcmp(diagnostic.message,"product binding has no owned transfer recipe"));
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    }
}

int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);
    static const char text[]=
        "fn consume(xs: move Array<i64>)->i64{xs.push(20);xs[0]=19;return xs[0]+xs[1]+1}\n"
        "fn loopCapture()->i64{var saved:fn()->i64=fn()->i64{return 0};var n=0;while(n<2){var xs=[42];if(n==1){return saved()};saved=fn()->i64{return xs[0]};n+=1};return 0}\n"
        "fn moved()->i64{var xs=[20,22];return consume(move xs)}\n"
        "fn copied()->i64{var xs=[20,22];consume(xs);return len(xs)}\n"
        "fn constant()->i64{const xs=[20,22];return consume(move xs)}\n"
        "fn cow()->i64{var xs=[20,22];const saved=xs;consume(move xs);return saved[0]+saved[1]}\n"
        "fn indirect()->i64{var xs=[20,22];const action:fn(move Array<i64>)->i64=consume;return action(move xs)}\n"
        "fn restored()->i64{var xs=[20,22];fn read()->i64{return xs[0]+xs[1]};consume(move xs);xs=[40,2];return read()}\n"
        "fn snapshot()->i64{var xs=[20,22];const first=xs[0];consume(move xs);fn identity(x:i64)->i64{return x};return identity(first)}\n"
        "fn text(value:move string)->string{value+=\"!\";return value}\n"
        "fn number(value:move i64)->i64{value+=1;return value}\n"
        "fn scalar()->i64{const n=41;return number(move n)}\n"
        "fn joined(flag:bool)->i64{var xs=[20,22];const out=flag?consume(move xs):consume(move xs);xs=[1,2];return out}\n"
        "fn destinations()->i64{var xs=[20,22];var target=move xs;fn read()->i64{return target[0]+target[1]};var replacement=[40,2];target=move replacement;return read()}\n"
        "fn echo(xs:move Array<i64>)->Array<i64>{return move xs}\n"
        "fn returned()->i64{var xs=[20,22];const result=echo(move xs);return result[0]+result[1]}\n"
        "print(moved(),copied(),constant(),cow(),indirect(),restored(),snapshot(),text(\"text\"),scalar(),joined(true),destinations(),returned())\n";
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    move_test_previous(&context);move_test_panic_graph(&context);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"move-source",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=sizeof(text)-1};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source status%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message?diagnostic.message:"");
    if(status!=XR_XIR_OK) {
        xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
        xr_compile_resources_release(context.resources);
        CHECK(status==XR_XIR_OK);return 1;
    }
    CHECK(status==XR_XIR_OK&&result.checked);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);uint32_t found=0;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if(function->name_length!=7||memcmp(function->name,"consume",7))continue;
        ++found;CHECK(function->parameter_count==1&&function->consume_parameter_count==1&&
            function->consume_parameters&&function->consume_parameters[0]==0&&
            xr_xir_type_is_array(module->types,function->parameters[0]));
    }
    CHECK(found==1);XrXirCheckedPacket packet={0};
    if(argc==1)move_test_refusals(&context);
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic issue={0};
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,&issue)==XR_XIR_OK);
    memset(packet.bytes,0x5a,packet.length);xr_xir_compile_checked_packet_free(&packet);
    status=xr_xir_compile_specialize(checked,&closed,&issue,NULL);
    fprintf(stderr,"MOVE Spec%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    status=xr_xir_compile_lower(closed,&target,NULL,&lowered,&issue);
    fprintf(stderr,"MOVE Lower%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    uint32_t selected=xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
    if(argc==2) {
        XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"move_values",8388608,&code)==XR_XIR_OK);
        FILE *file=fopen(argv[1],"wb");CHECK(file&&fwrite(code.text,1,code.length,file)==code.length);
        CHECK(fprintf(file,"\nconst uint32_t move_values_entry=%uu;\n",selected)>0&&!fclose(file));
        xr_xir_compile_c_source_free(&code);xr_xir_compile_artifact_free(lowered);
    } else {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
        move_test_run(program,selected,&context);
    }
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);CHECK(!runtime_live&&!runtime_bytes);return 0;
}
