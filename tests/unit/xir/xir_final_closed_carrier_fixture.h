#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_closed_carrier_fixture.h - Owned callable and Cell inputs
 *
 * KEY CONCEPT:
 *   Empty container facts never authorize a callable or erase extracted errors.
 *   Complete admission and real resource failures remain independent proofs.
 */
#ifndef XIR_FINAL_CLOSED_CARRIER_FIXTURE_H
#define XIR_FINAL_CLOSED_CARRIER_FIXTURE_H
enum { CC_ARRAY=258,CC_FN=259,CC_UNKNOWN_FN=260,CC_NULLABLE=261,CC_TUPLE=262,
    CC_ATOMIC=263,CC_STRUCT=264,CC_CLASS=265,CC_TASK=266,CC_ERROR_NULLABLE=267,
    CC_ARRAY_CELL=268,CC_ARRAY_TASK=269 };
typedef struct ClosedCarrierFixture {
    EdgeFixture edge;
    XrXirTypeNode nodes[14];
    XrXirNominalDeclaration declarations[3];
    XrXirNominalField field;
    XrXirType field_type,parameter;
    XrXirCallableParameter tuple[2];
} ClosedCarrierFixture;
static void cc_types(ClosedCarrierFixture *f) {
    EdgeFixture *e=&f->edge;
    memcpy(f->nodes,e->nodes,sizeof(e->nodes));
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)EE_ENUM};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)EE_ENUM};
    f->tuple[0]=(XrXirCallableParameter){(XrXirType)EE_ENUM,0,0};
    f->tuple[1]=(XrXirCallableParameter){XR_XIR_I64,0,0};
    f->nodes[6]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->tuple,.parameter_count=2};
    f->nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
    f->field_type=(XrXirType)EE_ENUM;
    f->field=(XrXirNominalField){.name={"payload",7},.type=(XrXirType)EE_ENUM};
    f->declarations[0]=e->nominal;
    f->declarations[1]=(XrXirNominalDeclaration){.module={"edges",5},.name={"Value",5},
        .exported=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_STRUCT};
    f->declarations[2]=(XrXirNominalDeclaration){.module={"edges",5},.name={"Object",6},
        .exported=1,.fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nodes[8]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=1,.fields=&f->field_type,.field_count=1}};
    f->nodes[9]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
        .nominal={.declaration=2,.fields=&f->field_type,.field_count=1}};
    f->nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64,.result=XR_XIR_NEVER};
    f->nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_ERROR};
    f->nodes[12]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)CC_ARRAY};
    f->nodes[13]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=(XrXirType)CC_ARRAY,.result=XR_XIR_NEVER};
    e->types.nodes=f->nodes;e->types.count=14;
    e->nominals.declarations=f->declarations;e->nominals.count=3;
}
static void cc_fixture(ClosedCarrierFixture *f,unsigned mode) {
    memset(f,0,sizeof(*f));EdgeFixture *e=&f->edge;
    ee_fixture(e,mode ? EE_JUMP : EE_BULK,false);cc_types(f);
    f->parameter=(XrXirType)CC_ARRAY;
    e->functions[1].parameters=&f->parameter;e->functions[1].parameter_count=1;
    if (!mode) {
        for (uint32_t i=0;i<512;++i)
            e->subject[i]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)CC_ARRAY,.args={0}};
        e->subject[544].args[0]=513;pc_reverse_blocks(e);return;
    }
    XrXirOp extract=XR_XIR_NULLABLE_UNWRAP;XrXirType result=(XrXirType)EE_ENUM;
    if (mode==5) e->functions[1].error_type=XR_XIR_ERROR;
    if (mode==1) f->parameter=(XrXirType)CC_NULLABLE;
    else if (mode==2) { f->parameter=(XrXirType)CC_TUPLE;extract=XR_XIR_TUPLE_FIELD; }
    else if (mode==3) { f->parameter=(XrXirType)CC_STRUCT;extract=XR_XIR_STRUCT_GET; }
    else if (mode==4) { f->parameter=(XrXirType)CC_CLASS;extract=XR_XIR_CLASS_GET; }
    else if (mode==5) { f->parameter=(XrXirType)CC_ERROR_NULLABLE;result=XR_XIR_ERROR; }
    else if (mode==6 || mode==7) {
        f->parameter=(XrXirType)(mode==6 ? CC_FN : CC_UNKNOWN_FN);
        e->functions[1].error_type=XR_XIR_NEVER;
        e->subject[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT};e->subject[1]=e->init;
        e->blocks[0]=(XrXirBlock){.count=2};
        e->functions[1].block_count=1;e->functions[1].instruction_count=2;return;
    } else {
        CHECK(mode==8);f->parameter=(XrXirType)CC_ARRAY_CELL;
        e->subject[0]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=(XrXirType)CC_ARRAY,.args={0}};
        e->subject[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        e->subject[2]=(XrXirInstruction){.op=XR_XIR_ARRAY_GET,.type=(XrXirType)EE_ENUM,.args={1,2}};
        e->subject[3]=(XrXirInstruction){.op=XR_XIR_THROW,.args={3}};
        e->blocks[0]=(XrXirBlock){.count=4};
        e->functions[1].block_count=1;e->functions[1].instruction_count=4;return;
    }
    e->subject[0]=(XrXirInstruction){.op=extract,.type=result,.args={0}};
    e->subject[1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
    e->blocks[0]=(XrXirBlock){.count=2};
    e->functions[1].block_count=1;e->functions[1].instruction_count=2;
}

#endif
