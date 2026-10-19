#include "xir_construction_fixture.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_instruction_cases.h - Exact built graphs for definition-time atomic admission
 */
#ifndef XIR_ATOMIC_INSTRUCTION_CASES_H
#define XIR_ATOMIC_INSTRUCTION_CASES_H
typedef struct AtomicInstructionFixture {
    XrXirNominalVariant variants[5];XrXirNominalDeclaration nominal;XrXirNominalTable table;
    XrXirTypeNode nodes[4];XrXirCallableParameter fields[2];XrXirTypes types;
    XrXirInstruction unit,main_ops[2],method_ops[6];XrXirBlock unit_block,main_block,method_block;
    XrXirFunction functions[4];XrXirType parameters[4];uint32_t operands[4],dependency;
    XrXirSourceModule modules[2];XrXirFunctionIdentity identities[4];XrXirDeclarations declarations;
    XrXirConstraint constraint;XrXirGeneric generics[4];XrXirModule module;
} AtomicInstructionFixture;
static void atomic_instruction_fixture(AtomicInstructionFixture *f,char *name,
    XrXirOp operation,XrXirType element,uint32_t marker,int known) {
    bool optional=known!=-3;
    *f=(AtomicInstructionFixture){0};f->nominal=ordering_declaration(name,f->variants);
    f->table=(XrXirNominalTable){.declarations=&f->nominal,.count=1};
    f->fields[0]=(XrXirCallableParameter){element,0};f->fields[1]=(XrXirCallableParameter){XR_XIR_BOOL,0};
    uint32_t span=marker?1u:0u;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=element,.parameter_span=span};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameter_span=span,.parameters=f->fields,.parameter_count=2};
    f->types=(XrXirTypes){f->nodes,4,&f->table,NULL};
    f->unit=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->main_ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->main_ops[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={0}};
    f->unit_block=(XrXirBlock){.count=1};f->main_block=(XrXirBlock){.count=2};
    f->functions[0]=(XrXirFunction){.name="root_init",.name_length=9,.result=XR_XIR_UNIT,
        .blocks=&f->unit_block,.block_count=1,.instructions=&f->unit,.instruction_count=1};
    f->functions[1]=f->functions[0];f->functions[1].name="ordering_init";f->functions[1].name_length=13;
    f->functions[2]=(XrXirFunction){.name="main",.name_length=4,.result=XR_XIR_I64,
        .blocks=&f->main_block,.block_count=1,.instructions=f->main_ops,.instruction_count=2};
    uint32_t required=operation==XR_XIR_ATOMIC_NEW?1u:xr_xir_atomic_required_operands(operation);
    uint32_t parameters=required+(optional && known==-1);
    f->parameters[0]=operation==XR_XIR_ATOMIC_NEW?element:(XrXirType)258;
    for(uint32_t a=1;a<required;++a)f->parameters[a]=element;
    if(optional && known==-1)f->parameters[required]=(XrXirType)257;
    uint32_t cursor=0,ordering_value=required;
    if(optional && known==-2){
        f->method_ops[cursor++]=(XrXirInstruction){.op=XR_XIR_NULLABLE_NONE,.type=(XrXirType)257};
        ordering_value=parameters;
    }
    if(optional && known>=0){
        f->method_ops[cursor++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)256,.immediate=known};
        f->method_ops[cursor++]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)257,.args={parameters}};
        f->method_ops[cursor++]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)257,.args={parameters+1}};
        ordering_value=parameters+2;
    }
    XrXirType result=operation==XR_XIR_ATOMIC_NEW?(XrXirType)258:
        operation==XR_XIR_ATOMIC_COMPARE_EXCHANGE?(XrXirType)259:
        operation==XR_XIR_ATOMIC_TOGGLE?XR_XIR_BOOL:operation==XR_XIR_ATOMIC_TO_STRING?XR_XIR_STRING:
        operation==XR_XIR_ATOMIC_STORE||operation==XR_XIR_ATOMIC_ADD||operation==XR_XIR_ATOMIC_SUB?XR_XIR_UNIT:element;
    uint32_t id=parameters+cursor;
    XrXirInstruction *op=&f->method_ops[cursor++];*op=(XrXirInstruction){.op=operation,.type=result};
    if(operation!=XR_XIR_ATOMIC_NEW){op->args[1]=required+optional;
        for(uint32_t a=0;a<required;++a)f->operands[a]=a;
        if(optional)f->operands[required]=ordering_value;
    }
    f->method_ops[cursor++]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT,.args={result==XR_XIR_UNIT?0:id}};
    f->method_block=(XrXirBlock){.count=cursor};
    f->functions[3]=(XrXirFunction){.name="atomic_method",.name_length=13,.parameters=f->parameters,.parameter_count=parameters,
        .result=result,.blocks=&f->method_block,.block_count=1,.instructions=f->method_ops,.instruction_count=cursor,
        .operands=operation==XR_XIR_ATOMIC_NEW?NULL:f->operands,.operand_count=operation==XR_XIR_ATOMIC_NEW?0:required+optional};
    f->dependency=1;f->modules[0]=(XrXirSourceModule){"root",4,&f->dependency,1,0};
    f->modules[1]=(XrXirSourceModule){name,(uint32_t)strlen(name),NULL,0,1};
    f->identities[0].module=0;f->identities[1].module=1;f->identities[2].exported=1;f->identities[3].exported=1;
    f->declarations=(XrXirDeclarations){.modules=f->modules,.module_count=2,.functions=f->identities,.entry_function=2};
    if(marker){f->constraint.markers=marker;f->generics[3]=(XrXirGeneric){.constraints=&f->constraint,.parameter_count=1};}
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,.types=&f->types,
        .declarations=&f->declarations,.generics=marker?f->generics:NULL,.linkage_kind=XR_XIR_PROGRAM};
}
typedef struct AtomicInstructionRun {XrXirOp operation;XrXirType element;bool optional;int known;} AtomicInstructionRun;
static XrXirStatus atomic_instruction_pipeline(const XrXirCompileContext *context,void *data) {
    AtomicInstructionRun *run=data;char *name=NULL;XrXirStatus status=ordering_factory(context,&name);
    AtomicInstructionFixture fixture;XrXirArtifact *checked=NULL,*read=NULL,*specialized=NULL,*lowered=NULL;
    XrXirCheckedPacket packet={0};
    if(status==XR_XIR_OK){atomic_instruction_fixture(&fixture,name,run->operation,run->element,0,run->optional?run->known:-3);
        status=xir_fixture_check(context, &fixture.module, &checked, NULL);if(status!=XR_XIR_OK)CHECK(!checked);}
    xr_compile_resources_free(name);name=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_write(checked,&packet,NULL);if(status!=XR_XIR_OK)CHECK(!packet.bytes&&!packet.length);}
    xr_xir_compile_artifact_free(checked);checked=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL);if(status!=XR_XIR_OK)CHECK(!read);}
    if(status==XR_XIR_OK){status=xr_xir_compile_specialize(read,&specialized,NULL);if(status!=XR_XIR_OK)CHECK(!specialized);}
    xr_xir_compile_artifact_free(read);read=NULL;
    if(status==XR_XIR_OK){status=xr_xir_compile_lower(specialized,&(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},&lowered,NULL);if(status!=XR_XIR_OK)CHECK(!lowered);}
    xr_xir_compile_artifact_free(specialized);specialized=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(lowered,NULL);
    if(status!=XR_XIR_OK&&source_program_compile_fail_at==SIZE_MAX)fprintf(stderr,"atomic op %s status%u\n",xr_xir_op_name(run->operation),status);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_checked_packet_free(&packet);return status;
}
#endif // XIR_ATOMIC_INSTRUCTION_CASES_H
