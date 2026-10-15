/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_rotation_fixture.h - Real recursive callable environments
 */
#ifndef XIR_INVOCATION_ROTATION_FIXTURE_H
#define XIR_INVOCATION_ROTATION_FIXTURE_H
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include <string.h>

typedef struct InvocationRotationFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;
    XrXirType parameters[5];uint32_t run_operands[5],rotate_operands[5];
    XrXirInstruction run[4],rotate[3],pure[2],root[3],init[4];
    XrXirBlock blocks[5];XrXirFunction functions[5];
    XrXirEffectParameter roles[5];XrXirRootTerm terms[5];
    XrXirRootValueIdentity values[2];XrXirFunctionEffectContract contracts[5];
    XrXirProvenance evidence;XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirSlot slot;XrXirDeclarations declarations;XrXirModule module;
} InvocationRotationFixture;

static inline void invocation_rotation_fixture(InvocationRotationFixture *f) {
    memset(f,0,sizeof(*f));
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->run[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=2};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=3};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,5}};
    f->run[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
    f->rotate[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=0};
    f->rotate[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=1,.args={0,5}};
    f->rotate[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={6,0}};
    f->pure[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->pure[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->root[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=(XrXirType)257,.immediate=0};
    f->root[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=XR_XIR_I64,.args={0,0}};
    f->root[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->init[1]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)257,.args={0,0}};
    f->init[2]=(XrXirInstruction){.op=XR_XIR_SLOT_INIT,.args={1,0},.immediate=0};
    f->init[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->run_operands[4]=1;
    for (uint32_t p=0;p<5;++p) {
        f->parameters[p]=(XrXirType)256;f->rotate_operands[p]=(p+1)%5;
        f->roles[p]=(XrXirEffectParameter){XR_XIR_EFFECT_PARAMETER_VARIABLE,
            XR_XIR_EFFECT_USE_FORWARD|(p?0:XR_XIR_EFFECT_USE_INVOKE)};
        f->terms[p]=(XrXirRootTerm){XR_XIR_ROOT_TERM_PARAMETER,p};
    }
    f->blocks[0]=(XrXirBlock){.count=4};f->blocks[1]=(XrXirBlock){.count=3};
    f->blocks[2]=(XrXirBlock){.count=2};f->blocks[3]=(XrXirBlock){.count=3};
    f->blocks[4]=(XrXirBlock){.count=4};
    f->functions[0]=(XrXirFunction){.name="run",.name_length=3,.result=XR_XIR_I64,
        .instructions=f->run,.instruction_count=4,.operands=f->run_operands,.operand_count=5};
    f->functions[1]=(XrXirFunction){.name="rotate",.name_length=6,.result=XR_XIR_I64,
        .parameters=f->parameters,.parameter_count=5,.instructions=f->rotate,.instruction_count=3,
        .operands=f->rotate_operands,.operand_count=5};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->pure,.instruction_count=2};
    f->functions[3]=(XrXirFunction){.name="root",.name_length=4,.result=XR_XIR_I64,
        .instructions=f->root,.instruction_count=3};
    f->functions[4]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .instructions=f->init,.instruction_count=4};
    for (uint32_t i=0;i<5;++i) { f->functions[i].blocks=&f->blocks[i];f->functions[i].block_count=1; }
    f->values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_AUTHENTIC_REF,(XrXirType)256};
    f->contracts[0]=(XrXirFunctionEffectContract){.formula={.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED},
        .values=f->values,.value_count=2};
    f->contracts[1]=(XrXirFunctionEffectContract){.parameter_count=5,.parameters=f->roles,
        .formula={.terms=f->terms,.term_count=5}};
    f->contracts[3].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->contracts[4].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    f->evidence=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=f->contracts,.contract_count=5};
    f->source=(XrXirSourceModule){.name="rotation",.name_length=8,.initializer=4};
    f->slot=(XrXirSlot){.type=(XrXirType)257,.mutable=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,
        .functions=f->identities,.slots=&f->slot,.slot_count=1,.entry_function=0};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.provenance=&f->evidence,.linkage_kind=XR_XIR_PROGRAM};
}

#endif
