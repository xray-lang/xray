/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_callable_array_transport.c - Owned raw storage and exact callable carriers
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)

typedef struct TransportFixture {
    XrXirTypeNode nodes[3];XrXirTypes types;
    XrXirInstruction main[24],pure[3],receiver[4],init;
    uint32_t operands[12];XrXirBlock blocks[4];XrXirFunction functions[4];
    XrXirRootValueIdentity values[12];XrXirFunctionEffectContract contracts[4];
    XrXirProvenance evidence;XrXirFunctionIdentity identities[4];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
    XrXirType parameter_type;XrXirEffectParameter parameter;
    uint32_t receiver_operand;XrXirCallableParameter callable_parameter;XrXirRootTerm term;
} TransportFixture;
static void transport_variant(TransportFixture *,uint32_t);
static void transport_fixture(TransportFixture *f,uint32_t requested) {
    uint32_t kind=requested<4?requested:0;
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,
        .result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)256};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->main[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=1};
    f->main[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={0,0}};
    uint32_t n=2,used=0;
    if(kind==1) {
        f->main[n++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
        f->main[n++]=(XrXirInstruction){.op=XR_XIR_ARRAY_REPEAT,.type=(XrXirType)257,.args={2,1}};
    } else {
        f->operands[0]=1;used=(kind==2?0:1);
        f->main[n++]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={0,used}};
        if(kind>=2) {
            f->main[n++]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)257,.args={2,0}};
            if(kind==2)f->main[n++]=(XrXirInstruction){.op=XR_XIR_ARRAY_PUSH,.args={3,1}};
            else {
                f->main[n++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
                f->operands[used]=3;f->operands[used+1]=4;f->operands[used+2]=1;
                f->main[n++]=(XrXirInstruction){.op=XR_XIR_ARRAY_SET,.args={used,3}};used+=3;
            }
        }
    }
    f->main[n++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->main[n]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={n-1,0}};++n;
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="main",.name_length=4,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .instructions=f->main,.instruction_count=n,.operands=used?f->operands:NULL,.operand_count=used};
    f->functions[1]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .instructions=f->pure,.instruction_count=2};
    f->functions[2]=(XrXirFunction){.name="init",.name_length=4,.error_type=XR_XIR_NEVER,
        .instructions=&f->init,.instruction_count=1};
    for(uint32_t i=0;i<3;++i){f->blocks[i]=(XrXirBlock){.count=f->functions[i].instruction_count};f->functions[i].blocks=&f->blocks[i];f->functions[i].block_count=1;}
    f->values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};
    f->contracts[0]=(XrXirFunctionEffectContract){.values=f->values,.value_count=2};
    f->contracts[2].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=3};
    f->source=(XrXirSourceModule){.name="transport",.name_length=9,.initializer=2};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=3,
        .declarations=&f->declarations,.provenance=&f->evidence,.linkage_kind=XR_XIR_PROGRAM};
    if(requested>=4)transport_variant(f,requested);
}

static void transport_variant(TransportFixture *f,uint32_t kind) {
    XrXirFunction *main=&f->functions[0];uint32_t n=0;
    if(kind==4) {
        f->main[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->main[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};n=4;
        main->operands=NULL;main->operand_count=0;
    }else if(kind==5) {
        f->main[0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
        f->main[1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0,0},.targets={1,2}};
        f->main[2]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=1};
        f->main[3]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={2,0}};
        f->main[4]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3,0}};
        f->main[5]=f->main[2];f->main[6]=f->main[3];f->main[6].args[0]=5;
        f->main[7]=f->main[4];
        f->main[8]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)256,.args={0,4}};
        f->main[9]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={4,1}};
        f->main[10]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->main[11]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={10,0}};
        const uint32_t operands[]={1,3,2,6,8};memcpy(f->operands,operands,sizeof(operands));main->operand_count=5;
        static const XrXirBlock blocks[]={
            {.first=0,.count=2},{.first=2,.count=3},{.first=5,.count=3},{.first=8,.count=4}};
        main->blocks=blocks;main->block_count=4;n=12;
        f->values[0]=(XrXirRootValueIdentity){2,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
        f->values[1]=(XrXirRootValueIdentity){3,XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};
        f->values[2]=(XrXirRootValueIdentity){5,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
        f->values[3]=(XrXirRootValueIdentity){6,XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};
        f->values[4]=(XrXirRootValueIdentity){8,XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};f->contracts[0].value_count=5;
    }else if(kind==6) {
        f->main[2]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)256,.args={1,0}};
        f->main[3]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=(XrXirType)256,.args={2,0}};
        f->main[4]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={0,1}};f->operands[0]=3;
        f->main[5]=f->main[0];f->main[6]=f->main[1];f->main[6].args[0]=5;
        f->main[7]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={2,6}};
        f->main[8]=f->main[3];
        f->main[9]=(XrXirInstruction){.op=XR_XIR_LOCAL_NEW,.type=(XrXirType)257,.args={4,0}};
        f->main[10]=(XrXirInstruction){.op=XR_XIR_ARRAY_PUSH,.args={9,8}};
        f->main[11]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->main[12]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={11,0}};n=13;
        const uint32_t positions[]={0,1,2,3,5,6,8};
        for(uint32_t i=0;i<7;++i)f->values[i]=(XrXirRootValueIdentity){positions[i],(positions[i]==0||positions[i]==5)?XR_XIR_EFFECT_VALUE_AUTHENTIC_REF:XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};
        f->contracts[0].value_count=7;
    }else if(kind==7) {f->main[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->main[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.args={0,1},.immediate=1};
        f->main[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={1,0}};
        f->main[3]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={1,1}};
        f->main[4]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->main[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};n=6;
        f->operands[0]=0;f->operands[1]=2;main->operand_count=2;
        f->parameter_type=XR_XIR_I64;f->functions[1].parameters=&f->parameter_type;f->functions[1].parameter_count=1;
        f->pure[0]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};f->functions[1].instruction_count=1;f->blocks[1].count=1;
        f->contracts[1].parameters=&f->parameter;f->contracts[1].parameter_count=1;
        f->values[0].instruction=1;f->values[1].instruction=2;
    }
    else {
        CHECK(kind==8);f->nodes[2]=f->nodes[0];f->nodes[2].flags=XR_XIR_CALLABLE_ROOT_NONE;f->types.count=3;
        f->main[0].type=(XrXirType)258;f->main[1].type=(XrXirType)258;
        f->values[0].declared_type=(XrXirType)258;f->values[1].declared_type=(XrXirType)258;
        f->main[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=3};
        f->main[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};f->operands[0]=1;main->operand_count=1;n=4;
        f->parameter_type=(XrXirType)258;f->receiver_operand=1;
        f->receiver[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={0,0}};
        f->receiver[1]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={0,1}};
        f->receiver[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
        f->receiver[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
        f->blocks[3]=(XrXirBlock){.count=4};
        f->functions[3]=(XrXirFunction){.name="receiver",.name_length=8,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
            .parameters=&f->parameter_type,.parameter_count=1,.instructions=f->receiver,.instruction_count=4,.blocks=&f->blocks[3],.block_count=1,.operands=&f->receiver_operand,.operand_count=1};
        f->parameter=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_FIXED,XR_XIR_EFFECT_USE_FIXED};
        f->values[2]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_FIXED,(XrXirType)256};
        f->contracts[3]=(XrXirFunctionEffectContract){.parameters=&f->parameter,.parameter_count=1,.values=&f->values[2],.value_count=1};
        f->evidence.contract_count=4;f->module.function_count=4;
    }
    main->instruction_count=n;if(kind!=5)f->blocks[0].count=n;
}

static void transport_raw(uint32_t kind) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats frame={0};CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
    TransportFixture fixture;transport_fixture(&fixture,kind);XrXirArtifact *source=NULL,*read=NULL,*closed=NULL;XrXirDiagnostic why={0};
    XrXirStatus status=xir_fixture_check(&context,&fixture.module,&source,&why);
    if(status!=XR_XIR_OK)fprintf(stderr,"raw%u check%u f%u b%u i%u\n",kind,status,why.function,why.block,why.instruction);
    CHECK(status==XR_XIR_OK && source);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);memset(&fixture,0xA5,sizeof(fixture));
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xA5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    status=xr_xir_compile_specialize(read,&closed,&why,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"raw%u specialize%u f%u b%u i%u\n",kind,status,why.function,why.block,why.instruction);
    CHECK(status==XR_XIR_OK && closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);const XrXirFunction *main=&module->functions[0];
    uint32_t references=0,carriers=0;
    for(uint32_t i=0;i<main->instruction_count;++i){
        const XrXirInstruction *op=&main->instructions[i];
        const XrXirTypeNode *bound=xr_xir_callable_signature(module->types,op->type);if(!bound)continue;
        CHECK(bound->flags==((kind==4||kind==8)?XR_XIR_CALLABLE_ROOT_NONE:XR_XIR_CALLABLE_ROOT_UNRESOLVED));
        CHECK(bound->result==XR_XIR_I64 && bound->element==XR_XIR_NEVER);
        if(op->op==XR_XIR_FUNCTION_REF)++references;else ++carriers;
        if(op->op==XR_XIR_COPY||op->op==XR_XIR_LOCAL_NEW||op->op==XR_XIR_LOCAL_READ)
            CHECK(xr_xir_operand_type(main,op->args[0])==op->type);
    }
    CHECK(references>0 && carriers>0);
    if(kind==8){uint32_t found=0;for(uint32_t f=0;f<module->function_count;++f){uint32_t declaration=UINT32_MAX;CHECK(xr_xir_compile_specialization_origin(closed,f,&declaration)==XR_XIR_OK);if(declaration!=3)continue;const XrXirFunction *body=&module->functions[f];CHECK(body->parameter_count==1&&xr_xir_callable_signature(module->types,body->parameters[0])->flags==XR_XIR_CALLABLE_ROOT_NONE);CHECK(body->instructions[0].op==XR_XIR_FUNCTION_WEAKEN&&body->instructions[0].args[0]==0&&xr_xir_callable_signature(module->types,body->instructions[0].type)->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);++found;}CHECK(found);}
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed,&why)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    XrCompileResourceStats done={0};CHECK(xr_compile_resources_stats(context.resources,&done)==XR_COMPILE_RESOURCE_OK && done.held_bytes==frame.held_bytes);
    xr_compile_resources_release(context.resources);printf("raw storage%u Template/packet retired, Closed exact carriers/frame PASS\n",kind);
}

static void transport_rejected(uint32_t variant){
 XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();XrCompileResourceStats frame={0},done={0};CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
 TransportFixture f;transport_fixture(&f,0);
 if(variant==0){f.functions[1].result=XR_XIR_BOOL;f.pure[0].op=XR_XIR_CONST_BOOL;f.pure[0].type=XR_XIR_BOOL;f.pure[0].immediate=1;}
 else if(variant==1)f.nodes[0].flags|=XR_XIR_CALLABLE_NO_SUSPEND;
 else if(variant==2)f.nodes[0].element=XR_XIR_ERROR;
 else if(variant==3){f.parameter_type=XR_XIR_I64;f.functions[1].parameters=&f.parameter_type;f.functions[1].parameter_count=1;f.pure[1].args[0]=1;f.contracts[1].parameters=&f.parameter;f.contracts[1].parameter_count=1;}
 else if(variant==4){f.nodes[2]=f.nodes[0];f.nodes[2].flags=XR_XIR_CALLABLE_ROOT_NONE;f.types.count=3;f.main[1].type=(XrXirType)258;f.values[1].declared_type=(XrXirType)258;}
 else{CHECK(variant==5);f.nodes[1]=f.nodes[0];f.callable_parameter=(XrXirCallableParameter){(XrXirType)256,0};f.nodes[1].parameters=&f.callable_parameter;f.nodes[1].parameter_count=1;f.nodes[1].flags=XR_XIR_CALLABLE_ROOT_NONE;f.nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)257};f.types.count=3;
  f.main[0].type=(XrXirType)257;f.main[1].type=(XrXirType)257;f.main[2].type=(XrXirType)258;f.values[0].declared_type=(XrXirType)257;f.values[1].declared_type=(XrXirType)257;
  f.parameter_type=(XrXirType)256;f.functions[1].parameters=&f.parameter_type;f.functions[1].parameter_count=1;
  f.pure[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};f.pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
  f.parameter=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,XR_XIR_EFFECT_USE_INVOKE};f.term=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,0};f.contracts[1].parameters=&f.parameter;f.contracts[1].parameter_count=1;f.contracts[1].formula.terms=&f.term;f.contracts[1].formula.term_count=1;
 }
 XrXirConstruction *construction=NULL;CHECK(xir_fixture_module_construction(&context,&f.module,&construction)==XR_XIR_OK);XrXirArtifact *output=NULL;XrXirDiagnostic why={0};XrXirStatus status=xr_xir_compile_check_v2(&context,&f.module,construction,&output,&why);xr_xir_compile_construction_free(construction);
 if(status!=XR_XIR_BAD_TYPE)fprintf(stderr,"negative%u status%u f%u i%u\n",variant,status,why.function,why.instruction);
 CHECK(status==XR_XIR_BAD_TYPE&&!output&&why.status==XR_XIR_BAD_TYPE);CHECK(xr_compile_resources_stats(context.resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);xr_compile_resources_release(context.resources);printf("strict raw negative%u exact BAD_TYPE/frame PASS\n",variant);
}


/* A call result keeps its callee's full type. Storage requires an actual
 * WEAKEN instruction; a receiving Array cannot retag that unrelated producer. */
static void transport_call_result(bool convert){
 XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrXirCompileContext context={0};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrCompileResourceStats frame={0},done={0};CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
 TransportFixture f;transport_fixture(&f,0);f.nodes[2]=f.nodes[0];f.nodes[2].flags=XR_XIR_CALLABLE_ROOT_NONE;f.types.count=3;
 f.receiver[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)258,.immediate=1};
 f.receiver[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
 f.blocks[3]=(XrXirBlock){.count=2};f.functions[3]=(XrXirFunction){.name="make",.name_length=4,.result=(XrXirType)258,.error_type=XR_XIR_NEVER,.instructions=f.receiver,.instruction_count=2,.blocks=&f.blocks[3],.block_count=1};
 f.main[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)258,.immediate=3};uint32_t next=1;
 if(convert)f.main[next++]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={0,0}};
 f.operands[0]=next-1;f.main[next++]=(XrXirInstruction){.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)257,.args={0,1}};
 f.main[next++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
 f.main[next]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={next-1,0}};++next;
 f.functions[0].instruction_count=next;f.blocks[0].count=next;
 f.values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)258};
 f.contracts[3]=(XrXirFunctionEffectContract){.values=&f.values[0],.value_count=1};
 f.values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_FIXED,(XrXirType)256};
 f.contracts[0]=(XrXirFunctionEffectContract){.values=convert?&f.values[1]:NULL,.value_count=convert?1:0};
 f.module.function_count=4;f.evidence.contract_count=4;
 XrXirConstruction *construction=NULL;CHECK(xir_fixture_module_construction(&context,&f.module,&construction)==XR_XIR_OK);
 XrXirArtifact *checked=NULL,*closed=NULL;XrXirDiagnostic why={0};XrXirStatus status=xr_xir_compile_check_v2(&context,&f.module,construction,&checked,&why);xr_xir_compile_construction_free(construction);
 if(!convert){CHECK(status==XR_XIR_BAD_TYPE&&!checked&&why.status==XR_XIR_BAD_TYPE);}
 else{
  if(status!=XR_XIR_OK)fprintf(stderr,"CALL producer check%u f%u i%u\n",status,why.function,why.instruction);
  CHECK(status==XR_XIR_OK&&checked);CHECK(xr_xir_compile_specialize(checked,&closed,&why,NULL)==XR_XIR_OK&&closed);
  const XrXirModule *m=xr_xir_compile_artifact_module(closed);const XrXirFunction *main=&m->functions[0];
  CHECK(main->instructions[0].op==XR_XIR_CALL&&main->instructions[0].immediate>=0&&(uint64_t)main->instructions[0].immediate<m->function_count);
  CHECK(main->instructions[0].type==m->functions[main->instructions[0].immediate].result);
  CHECK(xr_xir_callable_signature(m->types,main->instructions[0].type)->flags==XR_XIR_CALLABLE_ROOT_NONE);
  CHECK(main->instructions[1].op==XR_XIR_FUNCTION_WEAKEN&&main->instructions[1].args[0]==0);
  CHECK(main->instructions[1].type==xr_xir_array_element(m->types,main->instructions[2].type));
  CHECK(xr_xir_callable_signature(m->types,main->instructions[1].type)->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
  CHECK(xr_xir_compile_artifact_verify(closed,&why)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
 }
 CHECK(xr_compile_resources_stats(context.resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);xr_compile_resources_release(context.resources);
 printf("actual CALL producer, %s/frame PASS\n",convert?"real WEAKEN accepted without result retag":"missing conversion exact BAD_TYPE");
}

static uint32_t transport_named(const XrXirModule *m,const char *name){uint32_t found=UINT32_MAX;for(uint32_t f=0;f<m->function_count;++f)if(m->functions[f].name_length==strlen(name)&&!memcmp(m->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}CHECK(found!=UINT32_MAX);return found;}
static void transport_source_parameters(const char *stdlib){
 static const char text[]=
    "fn pure()->i64{return 7}\n"
    "fn store(value:fn()->i64)->Array<fn()->i64>{return [value]}\n"
    "fn fixed(value:fn()->i64)->Array<fn()->i64>{const erased:fn()->i64=value;return [erased]}\n"
    "export fn answer()->i64{const a=store(pure);const b=fixed(pure);return a[0]()+b[0]()}\n";
 XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();XrCompileResourceStats frame={0},done={0};CHECK(xr_compile_resources_stats(context.resources,&frame)==XR_COMPILE_RESOURCE_OK);
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"callable-array-parameters",NULL};XrXirSourceRequest request={session,NULL,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL};XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);xr_compile_session_free(session);
 if(status!=XR_XIR_OK)fprintf(stderr,"parameters Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);CHECK(status==XR_XIR_OK&&result.checked);
 const XrXirModule *source=xr_xir_compile_artifact_module(result.checked);uint32_t store=transport_named(source,"store"),fixed=transport_named(source,"fixed");
 CHECK(source->provenance&&source->provenance->contracts&&source->provenance->contracts[store].parameter_count==1);CHECK(source->provenance->contracts[store].parameters[0].kind==XR_XIR_EFFECT_PARAMETER_FIXED);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);memset(packet.bytes,0xA5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 XrXirDiagnostic why={0};status=xr_xir_compile_specialize(read,&closed,&why,NULL);if(status!=XR_XIR_OK)fprintf(stderr,"parameters specialize%u f%u b%u i%u\n",status,why.function,why.block,why.instruction);CHECK(status==XR_XIR_OK&&closed);xr_xir_compile_artifact_free(read);
 const XrXirModule *module=xr_xir_compile_artifact_module(closed);uint32_t stores=0,fixeds=0;
 for(uint32_t f=0;f<module->function_count;++f){uint32_t declaration=UINT32_MAX;CHECK(xr_xir_compile_specialization_origin(closed,f,&declaration)==XR_XIR_OK);if(declaration!=store&&declaration!=fixed)continue;const XrXirFunction *body=&module->functions[f];CHECK(body->parameter_count==1);bool array=false,weaken=false;for(uint32_t i=0;i<body->instruction_count;++i){const XrXirInstruction *op=&body->instructions[i];if(op->op==XR_XIR_ARRAY_NEW){XrXirType element=xr_xir_array_element(module->types,op->type);array=true;if(declaration==store)CHECK(body->parameters[0]==element);}if(op->op==XR_XIR_FUNCTION_WEAKEN){CHECK(xr_xir_compile_callable_weakening(&context,module->types,xr_xir_operand_type(body,op->args[0]),op->type)==XR_XIR_OK);weaken=true;}}
  CHECK(array);if(declaration==store)++stores;else{CHECK(weaken);++fixeds;}
 }
 CHECK(stores&&fixeds);XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower_take(&closed,&target,NULL,&lowered,&why)==XR_XIR_OK&&!closed);module=xr_xir_compile_artifact_module(lowered);uint32_t entry=transport_named(module,"answer"),initializer=module->declarations->entry_function;
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK&&!lowered);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
 CHECK(xr_xir_instance_start(instance,initializer,NULL,0)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);
 CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&value.type==XR_XIR_I64&&value.payload==14);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(value.type==XR_XIR_I64&&value.payload==14);xr_xir_value_drop(&value);
 CHECK(xr_compile_resources_stats(context.resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);xr_compile_resources_release(context.resources);puts("real Source CallPlan STORE fixed and declared WEAKEN, mapped closed formals, VM14/frame PASS");
}

int main(int argc,char **argv){CHECK(argc==2);for(uint32_t k=0;k<9;++k)transport_raw(k);for(uint32_t n=0;n<6;++n)transport_rejected(n);transport_call_result(false);transport_call_result(true);transport_source_parameters(argv[1]);return 0;}
