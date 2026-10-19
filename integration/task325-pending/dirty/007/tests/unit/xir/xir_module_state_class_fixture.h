/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_class_fixture.h - Four-slot owned class initialization graphs
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#ifndef XIR_MODULE_STATE_CLASS_FIXTURE_H
#define XIR_MODULE_STATE_CLASS_FIXTURE_H
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
typedef struct StateClassFixture {
    XrXirInstruction root[21],empty,main[2],ctor[3],replace[3],helper[3];
    uint32_t root_operands[8],ctor_operand,base,diamond[2];
    XrXirBlock blocks[5];XrXirFunction functions[8];XrXirFunctionIdentity identities[8];
    XrXirSourceModule modules[4];XrXirSlot slots[4];XrXirLiteral literal;
    XrXirNominalField field;XrXirNominalDeclaration nominal;XrXirNominalTable nominals;
    XrXirType field_type,class_type;XrXirTypeNode node;XrXirTypes types;
    XrXirDeclarations declarations;XrXirModule module;
} StateClassFixture;
static void state_class_fixture(StateClassFixture *f,uint32_t scenario,uint32_t rejected) {
    CHECK(scenario==5 || scenario==8);memset(f,0,sizeof(*f));
    f->class_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;f->field_type=XR_XIR_STRING;
    f->field=(XrXirNominalField){{"text",4},XR_XIR_STRING,XR_XIR_FIELD_MUTABLE};
    f->nominal=(XrXirNominalDeclaration){.module={rejected?"base":"root",4},.name={"OwnedState",10},
        .exported=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&f->field_type,1}};
    f->types=(XrXirTypes){&f->node,1,&f->nominals,NULL};
    f->empty=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->main[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}};
    f->main[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->ctor[0]=(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}};
    f->ctor[1]=(XrXirInstruction){XR_XIR_CLASS_NEW,f->class_type,{0,1},{0},0,{0}};
    f->ctor[2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
    f->replace[0]=f->ctor[0];
    f->replace[1]=(XrXirInstruction){XR_XIR_CLASS_SET,XR_XIR_UNIT,{0,1},{0},0,{0}};
    f->replace[2]=f->empty;
    f->helper[0]=(XrXirInstruction){XR_XIR_CLASS_GET,XR_XIR_STRING,{0},{0},0,{0}};
    f->helper[1]=f->ctor[0];f->helper[2]=f->empty;
    const uint32_t publication[]={1,0,3,2};uint32_t count=0,operands=0;
    for(uint32_t i=0;i<4;++i) {
        uint32_t value=count;
        f->root[count++]=(XrXirInstruction){XR_XIR_CALL,f->class_type,{0},{0},5,{0}};
        f->root[count++]=(XrXirInstruction){XR_XIR_COPY,f->class_type,{value},{0},0,{0}};
        f->root[count++]=(XrXirInstruction){XR_XIR_SLOT_INIT,XR_XIR_UNIT,{value+1},{0},publication[i],{0}};
        f->root_operands[operands]=value+1;
        f->root[count++]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_UNIT,{operands++,1},{0},6,{0}};
        if(scenario==8) {
            f->root_operands[operands]=value+1;
            f->root[count++]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_UNIT,{operands++,1},{0},7,{0}};
        }
    }
    f->root[count++]=f->empty;f->diamond[0]=1;f->diamond[1]=2;
    f->blocks[0]=(XrXirBlock){0,1,0,0};f->blocks[1]=(XrXirBlock){0,count,0,0};
    f->blocks[2]=(XrXirBlock){0,2,0,0};f->blocks[3]=(XrXirBlock){0,3,0,0};
    const char *names[]={"base_init","left_init","right_init","root_init","main","new","replace","consume"};
    for(uint32_t i=0;i<8;++i) {
        f->functions[i]=(XrXirFunction){names[i],(uint32_t)strlen(names[i]),NULL,0,XR_XIR_UNIT,
            &f->blocks[0],1,&f->empty,1,NULL,0};
        f->identities[i].module=i<4?i:(rejected && i>4?0:3);
    }
    uint32_t init=rejected?0:3;
    f->functions[init].blocks=&f->blocks[1];f->functions[init].instructions=f->root;
    f->functions[init].instruction_count=count;f->functions[init].operands=f->root_operands;
    f->functions[init].operand_count=operands;
    f->functions[4]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,&f->blocks[2],1,f->main,2,NULL,0};
    f->identities[4].exported=1;
    f->functions[5]=(XrXirFunction){"new",3,NULL,0,f->class_type,&f->blocks[3],1,f->ctor,3,&f->ctor_operand,1};
    f->identities[5].nominal_owner=1;f->identities[5].method_kind=XR_XIR_CONSTRUCTOR;
    f->functions[6]=(XrXirFunction){"replace",7,&f->class_type,1,XR_XIR_UNIT,&f->blocks[3],1,f->replace,3,NULL,0};
    f->identities[6].nominal_owner=1;f->identities[6].method_kind=XR_XIR_READ_METHOD;
    f->functions[7]=(XrXirFunction){"consume",7,&f->class_type,1,XR_XIR_UNIT,&f->blocks[3],1,f->helper,3,NULL,0};
    f->identities[7].nominal_owner=1;f->identities[7].method_kind=XR_XIR_READ_METHOD;
    f->modules[0]=(XrXirSourceModule){"base",4,NULL,0,0};
    f->modules[1]=(XrXirSourceModule){"left",4,&f->base,1,1};
    f->modules[2]=(XrXirSourceModule){"right",5,&f->base,1,2};
    f->modules[3]=(XrXirSourceModule){"root",4,f->diamond,2,3};
    for(uint32_t i=0;i<4;++i)f->slots[i]=(XrXirSlot){rejected?0:3,f->class_type,rejected==2?0:1};
    f->literal=(XrXirLiteral){"x",1};
    f->declarations=(XrXirDeclarations){f->modules,4,f->identities,f->slots,4,&f->literal,1,3,4,NULL};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,8,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}
static XrXirStatus state_class_build(const XrXirCompileContext *context,uint32_t scenario,
    XrXirArtifact **output) {
    StateClassFixture fixture;state_class_fixture(&fixture,scenario,0);
    XrXirArtifact *checked=NULL,*closed=NULL,*read=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context, &fixture.module, &checked, &diagnostic);
    CHECK(status==XR_XIR_OK || !checked);
    if(status==XR_XIR_OK) {
        status=xr_xir_compile_checked_write(checked,&packet,NULL);
        CHECK(status==XR_XIR_OK || (!packet.bytes && !packet.length));
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    CHECK(status==XR_XIR_OK || !read);
    xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(read,&closed,NULL);
    CHECK(status==XR_XIR_OK || !closed);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    CHECK(status==XR_XIR_OK || !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK)*output=lowered;else xr_xir_compile_artifact_free(lowered);
    return status;
}
#endif
