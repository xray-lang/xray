/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_stage_contract.c - Definition-owned constructor transactions
 *
 * KEY CONCEPT:
 *   The parent runs once and its complete prefix enters the real child places
 *   before later public-field overwrites or the single wrapper publication.
 */
#include "xir/xxir_class_stage_internal.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_generic.h"
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"stage contract %d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_construction_fixture.h"
enum {STAGE_BASE=XR_XIR_CONSTRUCTED_TYPE_BASE,STAGE_CHILD,STAGE_BASE_TUPLE,STAGE_CHILD_TUPLE};
enum {BASE_CTOR=2,BASE_HELPER,CHILD_CTOR,CHILD_HELPER,STAGE_ENTRY,STAGE_OTHER};
typedef struct StageFixture {
    XrXirModule module;XrXirTypes types;XrXirNominalTable nominals;
    XrXirNominalIdentity identities[2];XrXirNominalFieldIdentity fields[3];
    XrXirTypeNode nodes[4];XrXirType field_types[3],parameter;
    XrXirCallableParameter tuple[3];XrXirSourceModule modules[2];uint32_t dependency;
    XrXirFunctionIdentity declarations[9];XrXirDeclarations scope;
    XrXirFunction functions[9];XrXirBlock blocks[9];
    XrXirInstruction instructions[9][24];uint32_t operands[9][8];
    XrXirDefaultBinding default_record;XrXirDefaultTable defaults;
    XrXirImplementationBinding witness;XrXirImplementation implementation;
    XrXirImplementationTable implementations;
} StageFixture;
static void stage_function(StageFixture *f,uint32_t index,uint32_t count) {
    static const char *const names[]={"baseModule","childModule","BaseInit","BaseFields",
        "ChildInit","ChildFields","entry","ordinary","duplicate"};
    f->blocks[index]=(XrXirBlock){0,count,0,0};
    f->functions[index]=(XrXirFunction){.name=names[index],.name_length=(uint32_t)strlen(names[index]),
        .blocks=f->blocks+index,.block_count=1,.instructions=f->instructions[index],
        .instruction_count=count,.operands=f->operands[index],.error_type=XR_XIR_NEVER};
    f->declarations[index].module=index?1:0;
}
static void stage_wrapper(StageFixture *f,uint32_t index,uint32_t helper,XrXirType type,uint32_t fields) {
    stage_function(f,index,fields+3);XrXirFunction *body=f->functions+index;
    body->parameters=&f->parameter;body->parameter_count=1;body->result=type;body->operand_count=fields+1;
    f->instructions[index][0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=f->functions[helper].result,.args={0,1},.immediate=helper};
    f->operands[index][0]=0;
    for(uint32_t field=0;field<fields;++field) {
        f->instructions[index][field+1]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_I64,.args={1},.immediate=field};
        f->operands[index][field+1]=field+2;
    }
    f->instructions[index][fields+1]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=type,.args={1,fields}};
    f->instructions[index][fields+2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={fields+2}};
}
static void stage_fixture(StageFixture *f) {
    memset(f,0,sizeof(*f));f->parameter=XR_XIR_I64;
    for(uint32_t field=0;field<3;++field) {
        f->field_types[field]=XR_XIR_I64;f->tuple[field]=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
        f->fields[field]=(XrXirNominalFieldIdentity){{field==0?"x":field==1?"y":"z",1},XR_XIR_FIELD_MUTABLE};
    }
    f->identities[0]=(XrXirNominalIdentity){.module={"base",4},.name={"Base",4},.exported=1,
        .fields=f->fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS};
    f->identities[1]=(XrXirNominalIdentity){.module={"child",5},.name={"Child",5},.exported=1,
        .fields=f->fields,.field_count=3,.kind=XR_XIR_NOMINAL_CLASS,.parent=1};
    f->nominals=(XrXirNominalTable){.count=2,.identities=f->identities};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.fields=f->field_types,.field_count=2}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=1,.fields=f->field_types,.field_count=3}};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->tuple,.parameter_count=2};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->tuple,.parameter_count=3};
    f->types=(XrXirTypes){f->nodes,4,&f->nominals,NULL};
    f->modules[0]=(XrXirSourceModule){.name="base",.name_length=4};
    f->modules[1]=(XrXirSourceModule){.name="child",.name_length=5,.dependencies=&f->dependency,.dependency_count=1,.initializer=1};
    for(uint32_t i=0;i<8;++i) {
        stage_function(f,i,1);f->instructions[i][0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    }
    for(uint32_t i=BASE_CTOR;i<=CHILD_HELPER;++i) {
        f->functions[i].parameters=&f->parameter;f->functions[i].parameter_count=1;
        f->declarations[i]=(XrXirFunctionIdentity){.module=i<CHILD_CTOR?0:1,.exported=(i%2)==0,
            .nominal_owner=i<CHILD_CTOR?1:2,.member_access=(i%2)?XR_XIR_MEMBER_PRIVATE:XR_XIR_MEMBER_PUBLIC,
            .promises=XR_XIR_FUNCTION_NO_SUSPEND,.method_kind=(i%2)?XR_XIR_CONSTRUCTOR_FIELDS:XR_XIR_CONSTRUCTOR};
    }
    stage_function(f,BASE_HELPER,2);f->declarations[BASE_HELPER].module=0;
    f->functions[BASE_HELPER].parameters=&f->parameter;f->functions[BASE_HELPER].parameter_count=1;
    f->functions[BASE_HELPER].result=(XrXirType)STAGE_BASE_TUPLE;f->functions[BASE_HELPER].operand_count=2;
    f->instructions[BASE_HELPER][0]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)STAGE_BASE_TUPLE,.args={0,2}};
    f->instructions[BASE_HELPER][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    stage_function(f,CHILD_HELPER,17);f->functions[CHILD_HELPER].parameters=&f->parameter;
    f->functions[CHILD_HELPER].parameter_count=1;f->functions[CHILD_HELPER].result=(XrXirType)STAGE_CHILD_TUPLE;
    f->functions[CHILD_HELPER].operand_count=4;
    XrXirInstruction *ops=f->instructions[CHILD_HELPER];
    for(uint32_t i=0;i<3;++i)ops[i]=(XrXirInstruction){.op=XR_XIR_LOCAL_UNINIT,.type=XR_XIR_I64};
    ops[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)STAGE_BASE_TUPLE,.args={0,1},.immediate=BASE_HELPER};
    ops[4]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_I64,.args={4}};
    ops[5]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={1,5}};
    ops[6]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=XR_XIR_I64,.args={4},.immediate=1};
    ops[7]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={2,7}};
    ops[8]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    ops[9]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={1,9}};
    ops[10]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=35};
    ops[11]=(XrXirInstruction){.op=XR_XIR_LOCAL_WRITE,.args={3,11}};
    for(uint32_t i=0;i<3;++i)ops[12+i]=(XrXirInstruction){.op=XR_XIR_LOCAL_READ,.type=XR_XIR_I64,.args={i+1}};
    ops[15]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)STAGE_CHILD_TUPLE,.args={1,3}};
    ops[16]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={16}};
    f->operands[CHILD_HELPER][1]=13;f->operands[CHILD_HELPER][2]=14;f->operands[CHILD_HELPER][3]=15;
    stage_wrapper(f,BASE_CTOR,BASE_HELPER,(XrXirType)STAGE_BASE,2);f->declarations[BASE_CTOR].module=0;
    stage_wrapper(f,CHILD_CTOR,CHILD_HELPER,(XrXirType)STAGE_CHILD,3);
    stage_function(f,STAGE_ENTRY,4);f->functions[STAGE_ENTRY].result=XR_XIR_I64;f->functions[STAGE_ENTRY].operand_count=1;
    f->instructions[STAGE_ENTRY][0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->instructions[STAGE_ENTRY][1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=(XrXirType)STAGE_CHILD,.args={0,1},.immediate=CHILD_CTOR};
    f->instructions[STAGE_ENTRY][2]=(XrXirInstruction){.op=XR_XIR_CLASS_GET,.type=XR_XIR_I64,.args={1}};
    f->instructions[STAGE_ENTRY][3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};
    f->functions[STAGE_OTHER].parameters=&f->parameter;f->functions[STAGE_OTHER].parameter_count=1;
    f->scope=(XrXirDeclarations){.modules=f->modules,.module_count=2,.functions=f->declarations,.root_module=1,.entry_function=STAGE_ENTRY};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.linkage_kind=XR_XIR_PROGRAM,.types=&f->types,
        .functions=f->functions,.function_count=8,.declarations=&f->scope};
}
static void stage_expect(StageFixture *f,XrXirStatus expected) {
    unsigned char before[sizeof(*f)];memcpy(before,f,sizeof(*f));
    XrXirStatus status=xir_compile_class_stage_verify(&f->module,NULL);
    CHECK(status==expected && !memcmp(before,f,sizeof(*f)));
}
static void stage_refusals(void) {
    for(uint32_t kind=0;kind<20;++kind) {
        StageFixture f;stage_fixture(&f);XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        switch(kind) {
        case 0:f.declarations[CHILD_HELPER].method_kind=XR_XIR_NON_MEMBER;expected=XR_XIR_BAD_TYPE;break;
        case 1:f.declarations[BASE_HELPER].method_kind=XR_XIR_NON_MEMBER;expected=XR_XIR_BAD_TYPE;break;
        case 2:f.module.function_count=9;f.functions[8]=f.functions[CHILD_HELPER];f.declarations[8]=f.declarations[CHILD_HELPER];break;
        case 3:f.operands[BASE_CTOR][0]=1;break;
        case 4:f.instructions[CHILD_CTOR][1].immediate=1;break;
        case 5:f.operands[CHILD_CTOR][1]=3;break;
        case 6:f.instructions[CHILD_HELPER][4]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};break;
        case 7:f.instructions[CHILD_HELPER][5].args[0]=2;break;
        case 8:f.instructions[CHILD_HELPER][4].immediate=1;f.instructions[CHILD_HELPER][6].immediate=0;break;
        case 9:f.instructions[CHILD_HELPER][7].args[0]=1;break;
        case 10:f.instructions[CHILD_HELPER][5]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};break;
        case 11:f.instructions[CHILD_HELPER][0]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=(XrXirType)STAGE_CHILD_TUPLE,.args={0,3}};
            f.instructions[CHILD_HELPER][1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
            f.functions[CHILD_HELPER].instruction_count=f.blocks[CHILD_HELPER].count=2;
            memset(f.operands[CHILD_HELPER],0,sizeof(f.operands[CHILD_HELPER]));expected=XR_XIR_BAD_TYPE;break;
        case 12:f.instructions[CHILD_HELPER][17]=f.instructions[CHILD_HELPER][16];
            f.instructions[CHILD_HELPER][16]=f.instructions[CHILD_HELPER][3];
            f.functions[CHILD_HELPER].instruction_count=18;f.blocks[CHILD_HELPER].count=18;expected=XR_XIR_BAD_TYPE;break;
        case 13:f.blocks[CHILD_HELPER].count=16;f.functions[CHILD_HELPER].block_count=2;expected=XR_XIR_UNSUPPORTED;break;
        case 14:f.declarations[BASE_CTOR].member_access=XR_XIR_MEMBER_PRIVATE;expected=XR_XIR_BAD_TYPE;break;
        case 15:f.modules[1].dependency_count=0;expected=XR_XIR_BAD_TYPE;break;
        case 16:f.scope.entry_function=BASE_HELPER;expected=XR_XIR_BAD_TYPE;break;
        case 17:f.modules[0].initializer=BASE_HELPER;expected=XR_XIR_BAD_TYPE;break;
        case 18:f.default_record.function=BASE_HELPER;f.defaults=(XrXirDefaultTable){&f.default_record,1};
            f.module.defaults=&f.defaults;expected=XR_XIR_BAD_TYPE;break;
        case 19:f.witness.function=BASE_HELPER;f.implementation.bindings=&f.witness;f.implementation.binding_count=1;
            f.implementations=(XrXirImplementationTable){&f.implementation,1};f.scope.implementations=&f.implementations;expected=XR_XIR_BAD_TYPE;break;
        }
        stage_expect(&f,expected);
    }
    static const XrXirOp uses[]={XR_XIR_CALL,XR_XIR_FUNCTION_REF,XR_XIR_GO,XR_XIR_SCOPED_GO,
        XR_XIR_GENERATOR_NEW,XR_XIR_CLEANUP_REGISTER,XR_XIR_CLASS_METHOD_REF};
    for(uint32_t i=0;i<sizeof(uses)/sizeof(*uses);++i) {
        StageFixture f;stage_fixture(&f);f.instructions[STAGE_OTHER][0]=(XrXirInstruction){
            .op=uses[i],.type=(XrXirType)STAGE_BASE_TUPLE,.args={0,1},.immediate=BASE_HELPER};
        f.instructions[STAGE_OTHER][1]=(XrXirInstruction){.op=XR_XIR_RETURN};
        f.functions[STAGE_OTHER].instruction_count=f.blocks[STAGE_OTHER].count=2;
        f.functions[STAGE_OTHER].operand_count=1;stage_expect(&f,XR_XIR_BAD_TYPE);
        CHECK(xir_compile_class_stage_call(&f.module,STAGE_OTHER,0,NULL)==XR_XIR_BAD_TYPE);
    }
}
typedef struct StageCancel {uint32_t polls;} StageCancel;
static bool stage_cancel(const void *opaque) {return ++((StageCancel *)opaque)->polls==3;}
static void stage_owned_fixture(StageFixture *f) {
    stage_fixture(f);f->parameter=XR_XIR_STRING;
    static const XrXirLiteral literal={"field",5};f->scope.literals=&literal;f->scope.literal_count=1;
    for(uint32_t i=0;i<3;++i) {f->field_types[i]=XR_XIR_STRING;f->tuple[i].type=XR_XIR_STRING;}
    for(uint32_t fn=0;fn<f->module.function_count;++fn)
        for(uint32_t i=0;i<f->functions[fn].instruction_count;++i) {
            XrXirInstruction *op=&f->instructions[fn][i];
            if(op->type==XR_XIR_I64)op->type=XR_XIR_STRING;
            if(op->op==XR_XIR_CONST_INT) {op->op=XR_XIR_CONST_STRING;op->immediate=0;}
        }
    /* Program entry keeps its integer result while the fields own Strings. */
    f->instructions[STAGE_ENTRY][3]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->instructions[STAGE_ENTRY][4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3}};
    f->functions[STAGE_ENTRY].instruction_count=f->blocks[STAGE_ENTRY].count=5;
}
static void stage_empty_and_root(void) {
    StageFixture f;stage_fixture(&f);f.module.linkage_kind=XR_XIR_LIBRARY;
    f.module.function_count=4;f.types.count=f.nominals.count=1;f.scope.entry_function=UINT32_MAX;
    f.identities[0].fields=NULL;f.identities[0].field_count=0;
    f.nodes[0].nominal.fields=NULL;f.nodes[0].nominal.field_count=0;
    f.functions[BASE_HELPER].parameters=NULL;f.functions[BASE_HELPER].parameter_count=0;
    f.functions[BASE_HELPER].result=XR_XIR_UNIT;f.functions[BASE_HELPER].operand_count=0;
    f.functions[BASE_HELPER].instruction_count=f.blocks[BASE_HELPER].count=1;
    f.instructions[BASE_HELPER][0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f.functions[BASE_CTOR].parameters=NULL;f.functions[BASE_CTOR].parameter_count=0;
    f.functions[BASE_CTOR].operand_count=0;f.functions[BASE_CTOR].instruction_count=f.blocks[BASE_CTOR].count=3;
    f.instructions[BASE_CTOR][0]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=BASE_HELPER};
    f.instructions[BASE_CTOR][1]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=(XrXirType)STAGE_BASE};
    f.instructions[BASE_CTOR][2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    stage_expect(&f,XR_XIR_OK);
    CHECK(xir_compile_class_stage_call(&f.module,BASE_CTOR,0,NULL)==XR_XIR_OK);
    stage_fixture(&f);f.identities[1].parent=0;
    f.declarations[CHILD_HELPER]=(XrXirFunctionIdentity){.module=1};stage_function(&f,CHILD_HELPER,1);
    f.instructions[CHILD_HELPER][0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    XrXirBlock blocks[]={{0,2,0,0},{2,2,0,0},{4,2,0,0}};
    f.functions[CHILD_CTOR].blocks=blocks;f.functions[CHILD_CTOR].block_count=3;
    f.functions[CHILD_CTOR].instruction_count=6;f.functions[CHILD_CTOR].operand_count=3;
    memset(f.operands[CHILD_CTOR],0,sizeof(f.operands[CHILD_CTOR]));
    f.instructions[CHILD_CTOR][0]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    f.instructions[CHILD_CTOR][1]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}};
    f.instructions[CHILD_CTOR][2]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=(XrXirType)STAGE_CHILD,.args={0,3}};
    f.instructions[CHILD_CTOR][3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3}};
    f.instructions[CHILD_CTOR][4]=f.instructions[CHILD_CTOR][2];
    f.instructions[CHILD_CTOR][5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5}};
    stage_expect(&f,XR_XIR_OK);
}
static void stage_transition(StageFixture *f) {
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirNominalDeclaration declarations[2]={0};XrXirNominalField fields[2][3]={0};
    for(uint32_t n=0;n<2;++n) {
        const XrXirNominalIdentity *identity=f->identities+n;
        declarations[n]=(XrXirNominalDeclaration){.module=identity->module,.name=identity->name,
            .exported=identity->exported,.fields=fields[n],.field_count=identity->field_count,
            .kind=identity->kind,.flags=identity->flags,.native=identity->native,.parent=identity->parent};
        for(uint32_t field=0;field<identity->field_count;++field)
            fields[n][field]=(XrXirNominalField){identity->fields[field].name,
                f->nodes[n].nominal.fields[field],identity->fields[field].flags};
    }
    XrXirNominalTable definitions={.declarations=declarations,.count=2};
    XrXirTypes types=f->types;types.nominals=&definitions;XrXirModule built=f->module;built.types=&types;
    XrXirFunction functions[9];memcpy(functions,f->functions,sizeof(functions));
    for(uint32_t fn=0;fn<built.function_count;++fn)
        if(!functions[fn].operand_count)functions[fn].operands=NULL;
    built.functions=functions;
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&context,&built,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"complete class fixture status%u at %u/%u/%u\n",
        (unsigned)status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);
    CHECK(xir_compile_class_stage_verify(xr_xir_compile_artifact_module(checked),NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_module(lowered)->stage==XR_XIR_LOWERED);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    CHECK(xir_compile_class_stage_verify(module,NULL)==XR_XIR_OK);
    bool attacked=false;
    for(uint32_t fn=0;fn<module->function_count;++fn) {
        const XrXirFunctionIdentity *identity=&module->declarations->functions[fn];
        if(identity->method_kind!=XR_XIR_CONSTRUCTOR_FIELDS)continue;
        const XrXirNominalTable *table=module->types->nominals;uint32_t owner=identity->nominal_owner-1;
        CHECK(owner<table->count);
        if(!(table->declarations?table->declarations[owner].parent:table->identities[owner].parent))continue;
        XrXirInstruction *ops=(XrXirInstruction *)module->functions[fn].instructions;
        for(uint32_t i=0;i<module->functions[fn].instruction_count;++i)
            if(ops[i].op==XR_XIR_SCALAR_LOCAL_WRITE || ops[i].op==XR_XIR_OWNED_LOCAL_WRITE) {
                XrXirOp original=ops[i].op;ops[i].op=XR_XIR_LOCAL_WRITE;
                CHECK(xir_compile_class_stage_verify(module,NULL)==XR_XIR_BAD_STRUCTURE);
                ops[i].op=original;attacked=true;break;
            }
    }
    CHECK(attacked && xir_compile_class_stage_verify(module,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
}
static void stage_previous129_rejected(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==42 && XR_XIR_CHECKED_CONTRACT==130,"staged constructor acceptance identity");
    FILE *file=fopen(XR_CLASS_STAGE_PREVIOUS "/old129.xrchk","rb");CHECK(file);
    uint8_t packet[3228];CHECK(fread(packet,1,sizeof(packet),file)==sizeof(packet)&&fgetc(file)==EOF&&!ferror(file)&&!fclose(file));
    CHECK(!memcmp(packet,"XRCHK\0\0\0",8)&&packet[8]==42&&!packet[9]&&!packet[10]&&!packet[11]&&
        packet[12]==129&&!packet[13]&&!packet[14]&&!packet[15]);
    static const uint8_t expected[32]={0x29,0x1f,0xa6,0x89,0xcf,0x58,0x73,0x0e,0x67,0x3e,0x59,0x0e,0xcb,0xee,0x57,0x33,0xaa,0xd5,0x7a,0x85,0x3d,0x23,0x74,0xc8,0x19,0x2d,0xf0,0xc0,0x46,0x2a,0x05,0x24};uint8_t digest[32];xr_sha256(packet,sizeof(packet),digest);CHECK(!memcmp(digest,expected,32));
    XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet,32);xr_sha256_update(&sha,packet+64,sizeof(packet)-64);
    xr_sha256_final(&sha,digest);CHECK(!memcmp(digest,packet+32,32));
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);XrXirArtifact *output=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_checked_read(&context,packet,sizeof(packet),&output,&diagnostic)==XR_XIR_BAD_STRUCTURE&&!output);
    CHECK(diagnostic.status==XR_XIR_BAD_STRUCTURE&&xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&
        before.held_bytes==after.held_bytes&&before.allocation_count==after.allocation_count);xr_compile_resources_release(context.resources);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--raw-only")));
    stage_previous129_rejected();
    StageFixture f;stage_fixture(&f);stage_expect(&f,XR_XIR_OK);
    CHECK(xir_compile_class_stage_call(&f.module,BASE_CTOR,0,NULL)==XR_XIR_OK);
    CHECK(xir_compile_class_stage_call(&f.module,CHILD_HELPER,3,NULL)==XR_XIR_OK);
    StageCancel cancel={0};XrCompileRequestControl control={0};XrCompileRequestOptions options={stage_cancel,&cancel,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    CHECK(xir_compile_class_stage_verify(&f.module,&control)==XR_XIR_CANCELLED && cancel.polls==3);
    stage_expect(&f,XR_XIR_OK);stage_refusals();stage_empty_and_root();
    f.instructions[CHILD_HELPER][5].op=XR_XIR_SCALAR_LOCAL_WRITE;stage_expect(&f,XR_XIR_BAD_STRUCTURE);
    stage_fixture(&f);if(argc==1)stage_transition(&f);
    stage_owned_fixture(&f);stage_expect(&f,XR_XIR_OK);
    f.instructions[CHILD_HELPER][5].op=XR_XIR_OWNED_LOCAL_WRITE;stage_expect(&f,XR_XIR_BAD_STRUCTURE);
    stage_owned_fixture(&f);if(argc==1)stage_transition(&f);
    puts(argc==1?"Class field transactions: raw controls and real Check/Specialize/Lower PASS":
        "Class field transactions: raw-only controls PASS; artifact transitions NOT_RUN");
    return 0;
}
