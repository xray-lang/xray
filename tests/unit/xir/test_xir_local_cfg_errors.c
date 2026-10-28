/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_local_cfg_errors.c - Independent declaration-bound body reception
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_constraints.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_types.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);} } while (0)
#include "error_marker_type_cases.h"

typedef struct BodyWire { uint8_t *bytes;size_t length,position; } BodyWire;
static uint32_t body_wire_u32(BodyWire *wire) {
    CHECK(wire->position<=wire->length && wire->length-wire->position>=4);
    uint32_t value=0;
    for (unsigned i=0;i<4;++i) value|=(uint32_t)wire->bytes[wire->position+i]<<(8*i);
    wire->position+=4;return value;
}
static void body_wire_skip(BodyWire *wire,size_t count,size_t width) {
    CHECK(wire->position<=wire->length && width && count<=(wire->length-wire->position)/width);
    wire->position+=count*width;
}
static size_t body_wire_error(BodyWire *wire,uint32_t target,uint32_t functions) {
    CHECK(body_wire_u32(wire)<=XR_XIR_LIBRARY);
    CHECK(body_wire_u32(wire)==functions);CHECK(body_wire_u32(wire)==1);
    for (uint32_t f=0;f<functions;++f) {
        uint32_t name=body_wire_u32(wire);body_wire_skip(wire,name,1);
        uint32_t parameters=body_wire_u32(wire);body_wire_skip(wire,parameters,4);
        (void)body_wire_u32(wire);
        size_t error=wire->position;(void)body_wire_u32(wire);
        if (f==target) return error;
        uint32_t blocks=body_wire_u32(wire);body_wire_skip(wire,blocks,16);
        uint32_t instructions=body_wire_u32(wire);body_wire_skip(wire,instructions,40);
        uint32_t operands=body_wire_u32(wire);body_wire_skip(wire,operands,4);
    }
    CHECK(false);return 0;
}
static void body_packet_never(uint8_t *bytes,size_t length,uint32_t target,uint32_t functions) {
    CHECK(length>64 && !memcmp(bytes,"XRCHK\0\0\0",8));
    BodyWire wire={bytes,length,8};CHECK(body_wire_u32(&wire)==39);
    wire.position=64;size_t error=body_wire_error(&wire,target,functions);
    for (unsigned i=0;i<4;++i) bytes[error+i]=(uint8_t)((uint32_t)XR_XIR_NEVER>>(8*i));
    XrSHA256Context digest;xr_sha256_init(&digest);xr_sha256_update(&digest,bytes,32);
    xr_sha256_update(&digest,bytes+64,length-64);xr_sha256_final(&digest,bytes+32);
}
static uint32_t body_function(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (strlen(name)==module->functions[f].name_length &&
            !memcmp(module->functions[f].name,name,module->functions[f].name_length)) return f;
    CHECK(false);return UINT32_MAX;
}
typedef struct BodyErrorCase { const char *name,*text;XrXirOp propagation;bool caught,stateful; } BodyErrorCase;
static void body_execute(XrXirSourceProduct *product) {
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    uint32_t answer=body_function(xr_xir_compile_artifact_module(checked),"answer");
    uint32_t entry=xr_xir_compile_source_product_facts(product)->entry;
    xr_xir_compile_artifact_free(checked);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned n=0;n<2;++n) {
        CHECK(xr_xir_instance_start(instance,n?answer:entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==(n?42:0));xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void body_false_never(const BodyErrorCase *test) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"body-errors",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceText input={NULL,test->text,strlen(test->text)};XrXirSourceProduct *product=NULL;
    XrXirSourceProductDiagnostic source={0};
    XrXirStatus created=xr_xir_compile_source_product_build_text(&request,&input,&product,&source);
    if (created!=XR_XIR_OK) fprintf(stderr,"body source %s status%u %d:%d %s\n",test->name,(unsigned)created,
        source.source.line,source.source.column,source.source.message);
    CHECK(created==XR_XIR_OK);
    xr_compile_session_free(session);session=NULL;
    XrXirSourceProductPacketView packet={0};
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_SOURCE,&packet)==XR_XIR_OK);
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,&diagnostic)==XR_XIR_OK);
    const XrXirModule *original=xr_xir_compile_artifact_module(checked);
    uint32_t caller=body_function(original,"caller"),worker=body_function(original,"worker"),call=UINT32_MAX;
    CHECK(original->functions[worker].error_type!=XR_XIR_NEVER);
    CHECK(original->functions[caller].error_type!=XR_XIR_NEVER);
    for (uint32_t i=0;i<original->functions[caller].instruction_count;++i)
        if (original->functions[caller].instructions[i].op==test->propagation) call=i;
    CHECK(call!=UINT32_MAX);
    if (test->stateful) {
        uint32_t cells=0,locals=0,phis=0,back_edges=0;const XrXirFunction *body=&original->functions[caller];
        for (uint32_t i=0;i<body->instruction_count;++i) {
            XrXirOp op=body->instructions[i].op;
            cells+=op==XR_XIR_CELL_NEW || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_READ;
            locals+=op==XR_XIR_LOCAL_NEW || op==XR_XIR_LOCAL_WRITE || op==XR_XIR_LOCAL_READ;
            phis+=op==XR_XIR_PHI;
        }
        for (uint32_t b=0;b<body->block_count;++b) {
            const XrXirInstruction *end=&body->instructions[body->blocks[b].first+body->blocks[b].count-1];
            if (end->op==XR_XIR_JUMP) back_edges+=end->targets[0]<=b;
            else if (end->op==XR_XIR_BRANCH) back_edges+=end->targets[0]<=b || end->targets[1]<=b;
        }
        printf("%s actual CFG: Cell%u local%u PHI%u back-edge%u\n",test->name,cells,locals,phis,back_edges);
        CHECK(cells+locals>0);
        if (test->caught) CHECK(phis>0);else CHECK(back_edges>0 && cells>0);
    }
    XrXirFunction *functions=xr_malloc((size_t)original->function_count*sizeof(*functions));CHECK(functions);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    XrXirModule raw=*original;raw.functions=functions;raw.stage=XR_XIR_BUILT;raw.provenance=NULL;
    XrXirArtifact *built=NULL;
    XrXirStatus positive=xr_xir_compile_check_v2(&context,&raw,checked->construction,&built,&diagnostic);
    if (positive!=XR_XIR_OK) fprintf(stderr,"raw positive status%u f%u b%u i%u\n",(unsigned)positive,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(positive==XR_XIR_OK);
    xr_xir_compile_artifact_free(built);built=NULL;
    functions[caller].error_type=XR_XIR_NEVER;
    XrXirStatus raw_status=xr_xir_compile_check_v2(&context,&raw,checked->construction,&built,&diagnostic);
    XrXirDiagnostic raw_diagnostic=diagnostic;
    uint8_t *bytes=xr_malloc(packet.length);CHECK(bytes);memcpy(bytes,packet.bytes,packet.length);
    body_packet_never(bytes,packet.length,caller,original->function_count);
    XrXirArtifact *received=NULL;XrXirStatus packet_status=xr_xir_compile_checked_read(&context,bytes,packet.length,&received,&diagnostic);
    printf("%s false Never raw=%u f%u b%u i%u; Checked39=%u f%u b%u i%u; expected caller%u operation%u\n",
        test->name,(unsigned)raw_status,raw_diagnostic.function,raw_diagnostic.block,raw_diagnostic.instruction,
        (unsigned)packet_status,diagnostic.function,diagnostic.block,diagnostic.instruction,caller,call);
    fflush(stdout);
    if (test->caught) CHECK(raw_status==XR_XIR_OK && built && packet_status==XR_XIR_OK && received);
    else {
        CHECK(raw_status==XR_XIR_BAD_TYPE && !built && raw_diagnostic.function==caller && raw_diagnostic.instruction==call);
        CHECK(packet_status==XR_XIR_BAD_TYPE && !received && diagnostic.function==caller && diagnostic.instruction==call);
    }
    xr_xir_compile_artifact_free(built);xr_xir_compile_artifact_free(received);
    xr_free(bytes);xr_free(functions);xr_xir_compile_artifact_free(checked);
    body_execute(product);xr_xir_compile_source_product_diagnostic_free(&source);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    if (stats.held_bytes!=before.held_bytes) fprintf(stderr,"body resources before%llu after%llu\n",
        (unsigned long long)before.held_bytes,(unsigned long long)stats.held_bytes);
    CHECK(stats.held_bytes==before.held_bytes);xr_compile_resources_release(context.resources);
}
int main(void) {
    xr_test_suppress_dialogs();
    static const BodyErrorCase cases[]={
        {"direct declaration","enum Failure{Bad}\nfn worker() throws(Failure)->i64{return 42}\n"
            "fn caller() throws(Failure)->i64{return try worker()}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Failure){return 7}}\n",XR_XIR_CALL,false,false},
        {"indirect declaration","enum Failure{Bad}\nfn worker() throws(Failure)->i64{return 42}\n"
            "fn caller() throws(Failure)->i64{const f=worker;return try f()}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Failure){return 7}}\n",XR_XIR_CALL_INDIRECT,false,false},
        {"default declaration","enum Failure{Bad}\nfn worker() throws(Failure)->i64{return 42}\n"
            "fn selected(x:i64=try worker())->i64{return x}\n"
            "fn caller() throws(Failure)->i64{return try selected()}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Failure){return 7}}\n",XR_XIR_CALL_DEFAULT,false,false},
        {"generic declaration","enum Failure{Bad}\nfn worker<E:Error>(e:E) throws(E)->i64{return 42}\n"
            "fn caller() throws(Failure)->i64{return try worker<Failure>(Failure.Bad)}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Failure){return 7}}\n",XR_XIR_CALL,false,false},
        {"partial enum catch","enum Failure{One,Two}\nfn worker() throws(Failure)->i64{return 42}\n"
            "fn caller() throws(Failure)->i64{try{return worker()}catch(Failure.One){return 7}}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Failure){return 7}}\n",XR_XIR_THROW,false,false},
        {"unknown error catch","enum Failure{Bad}\nfn worker() throws(Error)->i64{return 42}\n"
            "fn caller() throws(Error)->i64{try{return worker()}catch(e:Failure){return 7}}\n"
            "export fn answer()->i64{try{return caller()}catch(e){return 7}}\n",XR_XIR_THROW,false,false},
        {"complete enum catch","enum Failure{One,Two}\nfn worker() throws(Failure)->i64{return 42}\n"
            "fn caller() throws(Failure)->i64{try{return worker()}catch(Failure.One){return 7}catch(Failure.Two){return 9}}\n"
            "export fn answer()->i64{try{return caller()}catch(e){return -1}}\n",XR_XIR_INVOKE,true,false},
        {"mutable erased catch","enum Failure{One,Two}\nfn worker() throws(Failure)->i64{return 1}\n"
            "fn caller() throws(Error)->i64{var error:Error=Failure.One;if(true){error=Failure.Two};"
            "try{const n=worker();throw error}catch(Failure.One){return 7}catch(Failure.Two){return 42}catch(e){return -1}}\n"
            "export fn answer()->i64{try{return caller()}catch(e){return -1}}\n",XR_XIR_INVOKE,true,true},
        {"loop callback cell","enum Failure{One}\nenum Other{Two}\nfn worker() throws(Error)->i64{return 42}\n"
            "fn caller() throws(Error)->i64{var error:Error=Failure.One;var count:i64=0;"
            "const change=fn(){error=Other.Two};while(count<2){change();count=count+1};"
            "try{throw error}catch(Failure.One){return 7}}\n"
            "export fn answer()->i64{try{return caller()}catch(e:Other){return 42}catch(e){return -1}}\n",XR_XIR_THROW,false,true}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) body_false_never(&cases[i]);
    for (unsigned mode=0;mode<16;++mode) marker_case(mode);
    puts("Local CFG error reception passed");return 0;
}
