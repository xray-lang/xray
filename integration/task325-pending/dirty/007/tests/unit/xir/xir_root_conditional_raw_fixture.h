/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_conditional_raw_fixture.h - Literal ordinary witness edges
 */
#ifndef XIR_ROOT_CONDITIONAL_RAW_FIXTURE_H
#define XIR_ROOT_CONDITIONAL_RAW_FIXTURE_H
#include "xir/xxir_implementation.h"

typedef struct ConditionalRawFixture {
    XrXirTypeNode nodes[6];
    XrXirCallableParameter callback;
    XrXirTypes types;
    XrXirInterfaceMethod method;
    XrXirInterfaceDeclaration interface;
    XrXirInterfaceTable interfaces;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirInterfaceApplication application;
    XrXirConstraint bound;
    XrXirType receiver, method_parameters[2], forward_parameters[2];
    uint32_t method_operands[2], forward_operands[2], caller_operands[2];
    XrXirInstruction method_ops[4], forward_ops[2], caller_ops[2], init_op;
    XrXirFunction functions[4];
    XrXirGeneric generics[4];
    XrXirFunctionIdentity identities[4];
    XrXirSourceModule source;
    XrXirImplementationBinding binding;
    XrXirImplementation implementation;
    XrXirImplementationTable implementations;
    XrXirSlot slot;
    XrXirDeclarations declarations;
    XrXirModule module;
} ConditionalRawFixture;

static void conditional_raw_fixture(ConditionalRawFixture *f, uint32_t mode) {
    memset(f,0,sizeof(*f));
    f->receiver=(XrXirType)256;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=8};
    f->callback=(XrXirCallableParameter){(XrXirType)257,0};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .parameters=&f->callback,.parameter_count=1,.flags=8};
    f->nodes[3]=f->nodes[1];f->nodes[3].flags=2;
    f->nodes[4]=f->nodes[1];f->nodes[4].flags=4;
    f->nodes[5]=f->nodes[1];f->nodes[5].flags=12;
    f->nominal=(XrXirNominalDeclaration){.module={"probe",5},.name={"Runner",6},.exported=1};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->method=(XrXirInterfaceMethod){{"apply",5},(XrXirType)258,0,0,NULL};
    f->interface=(XrXirInterfaceDeclaration){{"probe",5},{"Apply",5},1,NULL,0,NULL,0,&f->method,1};
    f->interfaces=(XrXirInterfaceTable){&f->interface,1};
    f->types=(XrXirTypes){f->nodes,6,&f->nominals,&f->interfaces};
    f->application=(XrXirInterfaceApplication){0,NULL,0};
    f->bound=(XrXirConstraint){0,&f->application,1};
    f->method_parameters[0]=f->receiver;f->method_parameters[1]=(XrXirType)257;
    f->forward_parameters[0]=XR_XIR_TYPE_PARAMETER_BASE;f->forward_parameters[1]=(XrXirType)257;
    f->method_operands[0]=1;f->forward_operands[0]=f->caller_operands[0]=0;
    f->forward_operands[1]=f->caller_operands[1]=1;
    f->method_ops[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1};
    f->method_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    uint32_t method_count=2;
    if (mode==1) {
        f->method_ops[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)257,.args={1,0}};
        f->method_ops[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2};
        f->method_ops[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};method_count=3;
    } else if (mode>=2) {
        f->method_operands[0]=0;f->method_operands[1]=1;
        f->method_ops[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.args={0,2},.immediate=2};
        f->method_ops[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2};
        f->method_ops[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};method_count=3;
        if (mode==3) {
            f->method_ops[1]=(XrXirInstruction){.op=XR_XIR_SLOT_STORE,.args={2,0}};
            f->method_ops[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
            f->method_ops[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={4,0}};method_count=4;
        }
    }
    if (mode==4) {
        f->method_ops[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=1};
        f->method_ops[1]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=XR_XIR_I64};
        f->method_ops[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};method_count=3;
    }
    XrXirType result=XR_XIR_I64;
    f->forward_ops[0]=(XrXirInstruction){.op=XR_XIR_CALL_REQUIREMENT,.type=result,.args={0,2}};
    f->forward_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->caller_ops[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=result,.args={0,2},.immediate=1,.type_arguments={0,1}};
    f->caller_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->init_op=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->functions[0]=(XrXirFunction){.name="apply",.name_length=5,.parameters=f->method_parameters,
        .parameter_count=2,.result=result,.instructions=f->method_ops,.instruction_count=method_count,
        .operands=f->method_operands,.operand_count=mode>=2 ? 2 : 1};
    f->functions[1]=(XrXirFunction){.name="forward",.name_length=7,.parameters=f->forward_parameters,
        .parameter_count=2,.result=result,.instructions=f->forward_ops,.instruction_count=2,
        .operands=f->forward_operands,.operand_count=2};
    f->functions[2]=(XrXirFunction){.name="caller",.name_length=6,.parameters=f->method_parameters,
        .parameter_count=2,.result=result,.instructions=f->caller_ops,.instruction_count=2,
        .operands=f->caller_operands,.operand_count=2};
    f->functions[3]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=&f->init_op,.instruction_count=1};
    f->generics[1]=(XrXirGeneric){.constraints=&f->bound,.parameter_count=1};
    f->generics[2]=(XrXirGeneric){.arguments=&f->receiver,.argument_count=1};
    f->identities[0]=(XrXirFunctionIdentity){.exported=1,.nominal_owner=1,.method_kind=XR_XIR_READ_METHOD};
    f->source=(XrXirSourceModule){"probe",5,NULL,0,3};
    f->binding=(XrXirImplementationBinding){f->application,0,0};
    f->implementation=(XrXirImplementation){0,f->application,&f->binding,1};
    f->implementations=(XrXirImplementationTable){&f->implementation,1};
    f->slot=(XrXirSlot){0,mode==4 ? XR_XIR_I64 : (XrXirType)257,1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .entry_function=2,.implementations=&f->implementations,
        .slots=mode>=3 ? &f->slot : NULL,.slot_count=mode>=3 ? 1 : 0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.generics=f->generics,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
#endif
