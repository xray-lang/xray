/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generation_join_fixture.h - Checked worker and producer without Source imports
 */
#ifndef XIR_GENERATION_JOIN_FIXTURE_H
#define XIR_GENERATION_JOIN_FIXTURE_H
#include "xir_construction_fixture.h"
static XrXirProgram *join_iterator_program(const XrXirCompileContext *context,uint32_t *entry) {
    XrXirType scalar=XR_XIR_I64;
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TASK,.element=XR_XIR_I64}};
    XrXirTypes types={.nodes=nodes,.count=2};
    XrXirInstruction init[]={{.op=XR_XIR_RETURN}};
    XrXirInstruction producer[]={{.op=XR_XIR_GENERATOR_YIELD,.args={0}},{.op=XR_XIR_RETURN}};
    uint32_t actual=0;
    XrXirInstruction worker[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=41},
        {.op=XR_XIR_GENERATOR_NEW,.type=256,.args={0,1},.immediate=1},
        {.op=XR_XIR_ITERATOR_NEXT,.args={1},.targets={1,2}},
        {.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={3}},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=-1},{.op=XR_XIR_RETURN,.args={6}}};
    XrXirInstruction run[]={
        {.op=XR_XIR_GO,.type=257,.immediate=2},
        {.op=XR_XIR_TASK_AWAIT,.args={0},.targets={1,2}},
        {.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64},{.op=XR_XIR_RETURN,.args={2}},
        {.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=-2},{.op=XR_XIR_RETURN,.args={5}}};
    XrXirBlock init_block={.count=1},producer_block={.count=2};
    XrXirBlock worker_blocks[]={{0,3,0,0},{3,2,0,0},{5,3,0,0}};
    XrXirBlock run_blocks[]={{0,2,0,0},{2,2,0,0},{4,3,0,0}};
    XrXirFunction functions[]={
        {.name="init",.name_length=4,.blocks=&init_block,.block_count=1,.instructions=init,.instruction_count=1},
        {.name="producer",.name_length=8,.result=256,.parameters=&scalar,.parameter_count=1,
            .blocks=&producer_block,.block_count=1,.instructions=producer,.instruction_count=2},
        {.name="worker",.name_length=6,.result=XR_XIR_I64,.blocks=worker_blocks,.block_count=3,
            .instructions=worker,.instruction_count=8,.operands=&actual,.operand_count=1},
        {.name="run",.name_length=3,.result=XR_XIR_I64,.blocks=run_blocks,.block_count=3,
            .instructions=run,.instruction_count=7}};
    XrXirFunctionIdentity identities[4]={{0}};
    identities[1].promises=XR_XIR_FUNCTION_GENERATOR;identities[3].exported=1;
    XrXirSourceModule source={.name="join",.name_length=4};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,.entry_function=3};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=functions,.function_count=4,
        .declarations=&declarations,.types=&types,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"join Checked=%u function=%u block=%u instruction=%u reason=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason);
    CHECK(status==XR_XIR_OK);
    XrXirProgram *program=join_program(checked,"run",entry);xr_xir_compile_artifact_free(checked);return program;
}
#endif
