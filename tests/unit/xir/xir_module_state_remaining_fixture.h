/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_remaining_fixture.h - Four-slot owned class initialization graphs
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#ifndef XIR_MODULE_STATE_REMAINING_FIXTURE_H
#define XIR_MODULE_STATE_REMAINING_FIXTURE_H
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_nominal.h"
typedef struct RemainingFixture {
    XrXirInstruction root[24],empty,main[4],ctor[3],replace[3],cause[2];
    uint32_t root_operands[8],ctor_operand,cause_operand,main_operand,base,diamond[2];
    XrXirBlock blocks[5];XrXirFunction functions[8];XrXirFunctionIdentity identities[8];
    XrXirSourceModule modules[4];XrXirSlot slots[4];XrXirLiteral literals[3];
    XrXirNominalField fields[2];XrXirNominalDeclaration nominals[2];XrXirNominalTable table;
    XrXirNominalVariant variant;XrXirType field_types[2],class_type,error_type;
    XrXirTypeNode nodes[2];XrXirTypes types;XrXirDeclarations declarations;XrXirModule module;
} RemainingFixture;
static bool remaining_yields(uint32_t scenario) {return scenario==7 || scenario==11 || scenario==12;}
static bool remaining_error(uint32_t scenario) {return scenario==9 || scenario==11;}
static void remaining_fixture(RemainingFixture *f,uint32_t scenario,uint32_t rejected) {
    CHECK(scenario==6 || scenario==7 || (scenario>=9 && scenario<=13));memset(f,0,sizeof(*f));
    f->class_type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;f->error_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1);
    f->field_types[0]=XR_XIR_STRING;f->field_types[1]=f->class_type;
    f->fields[0]=(XrXirNominalField){{"text",4},XR_XIR_STRING,XR_XIR_FIELD_MUTABLE};
    f->fields[1]=(XrXirNominalField){{"state",5},f->class_type,0};
    f->variant=(XrXirNominalVariant){{"Cause",5},0,1};
    f->nominals[0]=(XrXirNominalDeclaration){.module={rejected && rejected<3?"base":"root",4},
        .name={"OwnedState",10},.exported=1,.fields=&f->fields[0],.field_count=1,
        .kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals[1]=(XrXirNominalDeclaration){.module=f->nominals[0].module,.name={"InitFailure",11},
        .exported=1,.fields=&f->fields[1],.field_count=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=&f->variant,.variant_count=1};
    f->table=(XrXirNominalTable){f->nominals,2,NULL};
    for(uint32_t i=0;i<2;++i)f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={i,NULL,0,&f->field_types[i],1}};
    f->types=(XrXirTypes){f->nodes,2,&f->table,NULL};
    f->empty=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->main[0]=(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}};
    f->main[1]=(XrXirInstruction){XR_XIR_PRINT,XR_XIR_UNIT,{0,1},{0},0,{0}};
    f->main[2]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}};
    f->main[3]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}};
    f->ctor[0]=f->main[0];f->ctor[1]=(XrXirInstruction){XR_XIR_CLASS_NEW,f->class_type,{0,1},{0},0,{0}};
    f->ctor[2]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
    f->replace[0]=f->main[0];f->replace[1]=(XrXirInstruction){XR_XIR_CLASS_SET,XR_XIR_UNIT,{0,1},{0},0,{0}};
    f->replace[2]=f->empty;
    f->cause[0]=(XrXirInstruction){XR_XIR_ENUM_NEW,f->error_type,{0,1},{0},0,{0}};
    f->cause[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}};
    uint32_t count=0,operands=0,last=0;const uint32_t publication[]={1,0,3};
    if(scenario==11 || scenario==12)f->root[count++]=(XrXirInstruction){XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}};
    for(uint32_t i=0;i<3;++i) {
        uint32_t value=count;last=value+1;
        f->root[count++]=(XrXirInstruction){XR_XIR_CALL,f->class_type,{0},{0},5,{0}};
        f->root[count++]=(XrXirInstruction){XR_XIR_COPY,f->class_type,{value},{0},0,{0}};
        f->root[count++]=(XrXirInstruction){XR_XIR_SLOT_INIT,XR_XIR_UNIT,{value+1},{0},publication[i],{0}};
        f->root_operands[operands]=value+1;
        f->root[count++]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_UNIT,{operands++,1},{0},6,{0}};
    }
    if(remaining_error(scenario)) {
        if(rejected==3)f->root[count++]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{last},{0},0,{0}};
        else {
            f->root_operands[operands]=last;uint32_t made=count;
            f->root[count++]=(XrXirInstruction){XR_XIR_CALL,f->error_type,{operands++,1},{0},7,{0}};
            f->root[count++]=(XrXirInstruction){XR_XIR_ERROR_ERASE,XR_XIR_ERROR,{made},{0},0,{0}};
            f->root[count++]=(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{made+1},{0},0,{0}};
        }
    } else if(scenario==7) {
        f->root[count++]=(XrXirInstruction){XR_XIR_SUSPEND,XR_XIR_UNIT,{0},{0},0,{0}};f->root[count++]=f->empty;
    } else {
        uint32_t condition=count;f->root[count++]=(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}};
        uint32_t message=count;f->root[count++]=(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},scenario==6?1:scenario==13?0:2,{0}};
        f->root[count++]=(XrXirInstruction){XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,{condition,message},{0},0,{0}};
        f->root[count++]=f->empty;
    }
    f->diamond[0]=1;f->diamond[1]=2;f->blocks[0]=(XrXirBlock){0,1,0,0};
    f->blocks[1]=(XrXirBlock){0,count,0,0};f->blocks[2]=(XrXirBlock){0,4,0,0};
    f->blocks[3]=(XrXirBlock){0,3,0,0};f->blocks[4]=(XrXirBlock){0,2,0,0};
    const char *names[]={"base_init","left_init","right_init","root_init","main","new","replace","cause"};
    for(uint32_t i=0;i<8;++i) {
        f->functions[i]=(XrXirFunction){names[i],(uint32_t)strlen(names[i]),NULL,0,XR_XIR_UNIT,&f->blocks[0],1,&f->empty,1,NULL,0};
        f->identities[i].module=i<4?i:(rejected && rejected<3 && i>4?0:3);
    }
    uint32_t init=rejected && rejected<3?0:3;
    f->functions[init].blocks=&f->blocks[1];f->functions[init].instructions=f->root;
    f->functions[init].instruction_count=count;f->functions[init].operands=f->root_operands;f->functions[init].operand_count=operands;
    f->functions[4]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,&f->blocks[2],1,f->main,4,&f->main_operand,1};f->identities[4].exported=1;
    f->functions[5]=(XrXirFunction){"new",3,NULL,0,f->class_type,&f->blocks[3],1,f->ctor,3,&f->ctor_operand,1};
    f->identities[5].nominal_owner=1;f->identities[5].method_kind=XR_XIR_CONSTRUCTOR;
    f->functions[6]=(XrXirFunction){"replace",7,&f->class_type,1,XR_XIR_UNIT,&f->blocks[3],1,f->replace,3,NULL,0};
    f->identities[6].nominal_owner=1;f->identities[6].method_kind=XR_XIR_READ_METHOD;
    f->functions[7]=(XrXirFunction){"cause",5,&f->class_type,1,f->error_type,&f->blocks[4],1,f->cause,2,&f->cause_operand,1};
    f->identities[7].nominal_owner=2;f->identities[7].method_kind=XR_XIR_CONSTRUCTOR;
    f->modules[0]=(XrXirSourceModule){"base",4,NULL,0,0};f->modules[1]=(XrXirSourceModule){"left",4,&f->base,1,1};
    f->modules[2]=(XrXirSourceModule){"right",5,&f->base,1,2};f->modules[3]=(XrXirSourceModule){"root",4,f->diamond,2,3};
    for(uint32_t i=0;i<4;++i)f->slots[i]=(XrXirSlot){rejected && rejected<3?0:3,f->class_type,rejected==2?0:1};
    f->literals[0]=(XrXirLiteral){"x",1};f->literals[1]=(XrXirLiteral){"module initializer assertion",28};f->literals[2]=(XrXirLiteral){"",0};
    f->declarations=(XrXirDeclarations){f->modules,4,f->identities,f->slots,4,f->literals,3,3,4,NULL};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,8,&f->declarations,NULL,&f->types,NULL,XR_XIR_PROGRAM,NULL};
}
static XrXirStatus remaining_build(const XrXirCompileContext *context,uint32_t scenario,XrXirArtifact **output) {
    RemainingFixture fixture;remaining_fixture(&fixture,scenario,0);XrXirArtifact *checked=NULL,*closed=NULL,*read=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_check(context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"remaining %u Check=%u f=%u b=%u op=%u\n",scenario,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK || !checked);
    if(status==XR_XIR_OK) {status=xr_xir_compile_checked_write(checked,&packet,NULL);CHECK(status==XR_XIR_OK || (!packet.bytes && !packet.length));}
    xr_xir_compile_artifact_free(checked);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);
    CHECK(status==XR_XIR_OK || !read);xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(read,&closed,NULL);CHECK(status==XR_XIR_OK || !closed);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,&lowered,NULL);CHECK(status==XR_XIR_OK || !lowered);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(read);
    if(status==XR_XIR_OK)*output=lowered;else xr_xir_compile_artifact_free(lowered);return status;
}
#endif // XIR_MODULE_STATE_REMAINING_FIXTURE_H
