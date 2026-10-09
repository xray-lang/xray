/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_dependency_fixture.h - Literal summaries across reverse dependencies
 *
 * KEY CONCEPT:
 *   Unrelated scalar bodies do not change when a later callee acquires errors.
 *   A task dependency transports errors without granting parent authority.
 */
#ifndef XIR_ERROR_DEPENDENCY_FIXTURE_H
#define XIR_ERROR_DEPENDENCY_FIXTURE_H
#include "xir/xxir_nominal.h"
enum { ED_ENUM=256, ED_TASK=257, ED_FN=258, ED_CHAIN=32, ED_COUNT=ED_CHAIN+2, ED_SCALARS=512 };
typedef enum ErrorDependencyMode { ED_DIRECT, ED_CYCLE, ED_GO, ED_UNKNOWN, ED_UNREACHABLE } ErrorDependencyMode;
typedef struct ErrorDependencyFixture {
    XrXirNominalVariant variants[2];
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirTypeNode nodes[3];
    XrXirTypes types;
    XrXirType parameter, dynamic_parameters[2];
    XrXirInstruction init, scalar[ED_SCALARS+1], ops[ED_COUNT][5];
    XrXirBlock one, scalar_block, blocks[ED_COUNT], split[3];
    XrXirFunction functions[ED_COUNT];
    uint32_t operand;
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[ED_COUNT];
    XrXirDeclarations declarations;
    XrXirModule module;
} ErrorDependencyFixture;
static void ed_fixture(ErrorDependencyFixture *f, ErrorDependencyMode mode, bool large) {
    memset(f,0,sizeof(*f));
    f->variants[0]=(XrXirNominalVariant){{"First",5},0,0};
    f->variants[1]=(XrXirNominalVariant){{"Second",6},0,0};
    f->nominal=(XrXirNominalDeclaration){.module={"dependencies",12},.name={"Failure",7},
        .exported=1,.kind=XR_XIR_NOMINAL_ENUM,.variants=f->variants,.variant_count=2};
    f->nominals=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->nodes[0].kind=XR_XIR_TYPE_NOMINAL;
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_UNIT};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=3,.nominals=&f->nominals};
    f->parameter=(XrXirType)ED_ENUM;
    f->dynamic_parameters[0]=f->parameter;f->dynamic_parameters[1]=(XrXirType)ED_FN;
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN};f->one=(XrXirBlock){.count=1};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,
        .blocks=&f->one,.block_count=1,.instructions=&f->init,.instruction_count=1};
    uint32_t scalars=large?ED_SCALARS:8;
    for(uint32_t i=0;i<scalars;++i)f->scalar[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    f->scalar[scalars]=f->init;f->scalar_block=(XrXirBlock){.count=scalars+1};
    f->functions[1]=(XrXirFunction){.name="independent",.name_length=11,.result=XR_XIR_UNIT,
        .blocks=&f->scalar_block,.block_count=1,.instructions=f->scalar,.instruction_count=scalars+1};
    for(uint32_t i=2;i<ED_COUNT;++i){
        f->ops[i][0]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=i+1};
        f->ops[i][1]=f->init;f->blocks[i]=(XrXirBlock){.count=2};
        f->functions[i]=(XrXirFunction){.name="chain",.name_length=5,.parameters=&f->parameter,.parameter_count=1,
            .result=XR_XIR_UNIT,.blocks=&f->blocks[i],.block_count=1,.instructions=f->ops[i],.instruction_count=2,
            .operands=&f->operand,.operand_count=1};
    }
    f->ops[ED_COUNT-1][0]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
    f->blocks[ED_COUNT-1].count=1;f->functions[ED_COUNT-1].instruction_count=1;
    f->functions[ED_COUNT-1].operands=NULL;f->functions[ED_COUNT-1].operand_count=0;
    if(mode==ED_CYCLE){
        f->ops[ED_COUNT-1][0]=(XrXirInstruction){.op=XR_XIR_CALL,.args={0,1},.immediate=2};
        f->ops[ED_COUNT-1][1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
        f->blocks[ED_COUNT-1].count=2;f->functions[ED_COUNT-1].instruction_count=2;
        f->functions[ED_COUNT-1].operands=&f->operand;f->functions[ED_COUNT-1].operand_count=1;
    }
    if(mode==ED_GO){
        f->ops[2][0]=(XrXirInstruction){.op=XR_XIR_GO,.type=(XrXirType)ED_TASK,.args={0,1},.immediate=3};
        f->ops[2][1]=(XrXirInstruction){.op=XR_XIR_TASK_AWAIT,.args={1},.targets={1,2}};
        f->ops[2][2]=f->init;
        f->ops[2][3]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=1};
        f->ops[2][4]=(XrXirInstruction){.op=XR_XIR_THROW,.args={4}};
        f->split[0]=(XrXirBlock){.count=2};f->split[1]=(XrXirBlock){.first=2,.count=1};
        f->split[2]=(XrXirBlock){.first=3,.count=2};
        f->functions[2].blocks=f->split;f->functions[2].block_count=3;f->functions[2].instruction_count=5;
    }
    if(mode==ED_UNKNOWN){
        f->functions[2].parameters=f->dynamic_parameters;f->functions[2].parameter_count=2;
        f->functions[2].operands=NULL;f->functions[2].operand_count=0;
        f->ops[2][0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.immediate=1};
    }
    if(mode==ED_UNREACHABLE){
        f->ops[2][0]=f->init;f->ops[2][1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
        f->split[0]=(XrXirBlock){.count=1};f->split[1]=(XrXirBlock){.first=1,.count=1};
        f->functions[2].blocks=f->split;f->functions[2].block_count=2;
        f->functions[2].operands=NULL;f->functions[2].operand_count=0;
    }
    f->source=(XrXirSourceModule){.name="dependencies",.name_length=12,.initializer=0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=ED_COUNT,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
#endif // XIR_ERROR_DEPENDENCY_FIXTURE_H
