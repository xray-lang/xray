/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_owned_fixture.h - Producer-owned class declaration and body fixture
 *
 * KEY CONCEPT:
 *   Identity, private fields and packed body survive destruction of producer metadata.
 */
#ifndef XIR_CLASS_OWNED_FIXTURE_H
#define XIR_CLASS_OWNED_FIXTURE_H
typedef struct ClassOwnedFixture {
    char name[7];
    XrXirNominalField fields[2];
    XrXirNominalDeclaration declaration;
    XrXirNominalTable nominals;
    XrXirType field_types[2],constructor_parameters[2],method_parameters[2];
    XrXirTypeNode node;
    XrXirTypes types;
    XrXirInstruction init[1],constructor[2],method[4],entry[7],generic_return[1];
    uint32_t constructor_operands[2],entry_operands[5];
    XrXirBlock blocks[5];
    XrXirFunction functions[5];
    XrXirFunctionIdentity identities[5];
    XrXirGeneric generics[5];
    XrXirConstraint constraint;
    XrXirType generic_parameter,generic_argument;
    XrXirLiteral literal;
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} ClassOwnedFixture;
static void class_owned_fixture(ClassOwnedFixture *f) {
    memset(f,0,sizeof(*f));memcpy(f->name,"Counter",7);
    XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    f->fields[0]=(XrXirNominalField){{"value",5},XR_XIR_I64,XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_MUTABLE};
    f->fields[1]=(XrXirNominalField){{"label",5},XR_XIR_STRING,0};
    f->declaration=(XrXirNominalDeclaration){.module={"root",4},.name={f->name,7},.exported=1,
        .fields=f->fields,.field_count=2,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals=(XrXirNominalTable){&f->declaration,1,NULL};
    f->field_types[0]=XR_XIR_I64;f->field_types[1]=XR_XIR_STRING;
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,f->field_types,2}};
    f->types=(XrXirTypes){&f->node,1,&f->nominals,NULL};
    f->constructor_parameters[0]=XR_XIR_I64;f->constructor_parameters[1]=XR_XIR_STRING;
    f->method_parameters[0]=type;f->method_parameters[1]=XR_XIR_I64;
    f->constructor_operands[0]=0;f->constructor_operands[1]=1;
    for(uint32_t i=0;i<5;++i) f->entry_operands[i]=i;
    f->init[0]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->constructor[0]=(XrXirInstruction){XR_XIR_CLASS_NEW,type,{0,2},{0},0,{0}};
    f->constructor[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}};
    f->method[0]=(XrXirInstruction){XR_XIR_CLASS_GET,XR_XIR_I64,{0},{0},0,{0}};
    f->method[1]=(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{2,1},{0},0,{0}};
    f->method[2]=(XrXirInstruction){XR_XIR_CLASS_SET,XR_XIR_UNIT,{0,3},{0},0,{0}};
    f->method[3]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{3},{0},0,{0}};
    f->entry[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},40,{0}};
    f->entry[1]=(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},0,{0}};
    f->entry[2]=(XrXirInstruction){XR_XIR_CALL,type,{0,2},{0},1,{0}};
    f->entry[3]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}};
    f->entry[4]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{2,2},{0},2,{0}};
    f->entry[5]=(XrXirInstruction){XR_XIR_CALL,XR_XIR_I64,{4,1},{0},4,{0,1}};
    f->entry[6]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{5},{0},0,{0}};
    f->generic_parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;f->generic_argument=XR_XIR_I64;
    f->constraint.markers=XR_XIR_CONSTRAINT_SENDABLE;
    f->generics[4]=(XrXirGeneric){&f->constraint,1,NULL,0};
    f->generics[3]=(XrXirGeneric){NULL,0,&f->generic_argument,1};
    f->generic_return[0]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    f->blocks[0]=(XrXirBlock){0,1,0,0};f->blocks[1]=(XrXirBlock){0,2,0,0};
    f->blocks[2]=(XrXirBlock){0,4,0,0};f->blocks[3]=(XrXirBlock){0,7,0,0};f->blocks[4]=(XrXirBlock){0,1,0,0};
    f->functions[0]=(XrXirFunction){"init",4,NULL,0,XR_XIR_UNIT,&f->blocks[0],1,f->init,1,NULL,0};
    f->functions[1]=(XrXirFunction){"new",3,f->constructor_parameters,2,type,&f->blocks[1],1,f->constructor,2,f->constructor_operands,2};
    f->functions[2]=(XrXirFunction){"add",3,f->method_parameters,2,XR_XIR_I64,&f->blocks[2],1,f->method,4,NULL,0};
    f->functions[3]=(XrXirFunction){"entry",5,NULL,0,XR_XIR_I64,&f->blocks[3],1,f->entry,7,f->entry_operands,5};
    f->functions[4]=(XrXirFunction){"id",2,&f->generic_parameter,1,f->generic_parameter,&f->blocks[4],1,f->generic_return,1,NULL,0};
    f->identities[1]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
    f->identities[2]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_READ_METHOD};
    f->literal=(XrXirLiteral){"counter",7};f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .literals=&f->literal,.literal_count=1,.entry_function=3};
    f->module=(XrXirModule){XR_XIR_BUILT,f->functions,5,&f->declarations,f->generics,&f->types,NULL, XR_XIR_PROGRAM};
}
#endif // XIR_CLASS_OWNED_FIXTURE_H
