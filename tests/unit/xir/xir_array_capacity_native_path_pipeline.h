/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_native_path_pipeline.h - Explicit READ path capacity graphs
 *
 * KEY CONCEPT:
 *   Source may load a temporary before querying capacity. These complete Built
 *   graphs independently exercise the permitted READ projection role instead.
 */
#ifndef XIR_ARRAY_CAPACITY_NATIVE_PATH_PIPELINE_H
#define XIR_ARRAY_CAPACITY_NATIVE_PATH_PIPELINE_H
#include "xir_array_metadata_fixture.h"
typedef struct CapacityNativePathFixture {
    XirArrayMetadataFixture metadata;
    XrXirInstruction entry[2];
    XrXirBlock entry_block;
    XrXirFunction functions[4];
    XrXirFunctionIdentity identities[4];
} CapacityNativePathFixture;
static void capacity_native_path_fixture(CapacityNativePathFixture *path) {
    memset(path,0,sizeof(*path));
    XirArrayMetadataFixture *fixture=&path->metadata;
    xir_array_metadata_init(fixture);
    const XrXirType inner=(XrXirType)256,outer=(XrXirType)258;
    fixture->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=inner};
    fixture->ops[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},13,{0}};
    fixture->ops[1]=(XrXirInstruction){XR_XIR_ARRAY_WITH_CAPACITY,inner,{0},{0},0,{0}};
    fixture->ops[2]=(XrXirInstruction){XR_XIR_ARRAY_NEW,outer,{0,1},{0},0,{0}};
    fixture->ops[3]=(XrXirInstruction){XR_XIR_LOCAL_NEW,outer,{2},{0},0,{0}};
    fixture->ops[4]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
    fixture->ops[5]=(XrXirInstruction){XR_XIR_INDEX_PLACE,inner,{3,4},{0},0,{0}};
    fixture->ops[6]=(XrXirInstruction){XR_XIR_ARRAY_CAPACITY,XR_XIR_I64,{5},{0},0,{0}};
    fixture->ops[7]=(XrXirInstruction){XR_XIR_GE_INT,XR_XIR_BOOL,{6,0},{0},0,{0}};
    fixture->ops[8]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{7},{0},0,{0}};
    fixture->operands[0]=1;
    fixture->blocks[1].count=9;fixture->functions[1].instruction_count=9;
    fixture->functions[1].result=XR_XIR_BOOL;fixture->functions[1].operand_count=1;
    memcpy(&fixture->ops[9],fixture->ops,9*sizeof(*fixture->ops));
    fixture->ops[13].immediate=1;
    fixture->blocks[2]=(XrXirBlock){0,9,0,0};
    fixture->functions[2]=(XrXirFunction){"badIndex",8,NULL,0,XR_XIR_BOOL,
        &fixture->blocks[2],1,&fixture->ops[9],9,fixture->operands,1};
    path->entry[0]=(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}};
    path->entry[1]=(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    path->entry_block=(XrXirBlock){0,2,0,0};
    memcpy(path->functions,fixture->functions,sizeof(fixture->functions));
    memcpy(path->identities,fixture->identities,sizeof(fixture->identities));
    path->identities[1].exported=1;path->identities[2].exported=1;
    path->functions[3]=(XrXirFunction){"main",4,NULL,0,XR_XIR_I64,
        &path->entry_block,1,path->entry,2,NULL,0};
    fixture->declarations.functions=path->identities;fixture->declarations.entry_function=3;
    fixture->module.functions=path->functions;fixture->module.function_count=4;
}
static void capacity_native_emit_path(const XrXirCompileContext *context,const char *path) {
    CapacityNativePathFixture fixture;capacity_native_path_fixture(&fixture);
    XrXirArtifact *checked=NULL,*replay=NULL,*specialized=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};
    const XrXirModule *module=&fixture.metadata.module;
    XrXirStatus status=xr_xir_compile_check(context,module,&checked,&diagnostic);
    if(status!=XR_XIR_OK){
        fprintf(stderr,"NATIVE_CAPACITY_PATH_CHECK status=%u diagnosticStatus=%u function=%u block=%u instruction=%u reason=%u entry=%u entryResult=%u\n",
            (unsigned)status,(unsigned)diagnostic.status,diagnostic.function,diagnostic.block,
            diagnostic.instruction,(unsigned)diagnostic.reason,module->declarations->entry_function,
            (unsigned)module->functions[module->declarations->entry_function].result);
        if(diagnostic.function<module->function_count){
            const XrXirFunction *function=&module->functions[diagnostic.function];
            if(diagnostic.instruction<function->instruction_count){
                const XrXirInstruction *instruction=&function->instructions[diagnostic.instruction];
                fprintf(stderr,"NATIVE_CAPACITY_PATH_OP op=%s type=%u args=%u/%u immediate=%lld\n",
                    xr_xir_op_name(instruction->op),(unsigned)instruction->type,
                    instruction->args[0],instruction->args[1],(long long)instruction->immediate);
            }
        }
    }
    CHECK(status==XR_XIR_OK);
    memset(&fixture,0xcc,sizeof(fixture));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&replay,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(replay,&specialized,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(replay);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(specialized);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"capacity_native_path",1048576,&c)==XR_XIR_OK);
    CHECK(c.text&&c.length&&!c.text[c.length]&&!strstr(c.text,"({")&&strstr(c.text,"xr_xir_instance_path_capacity("));
    xr_xir_compile_artifact_free(lowered);
    FILE *file=fopen(path,"wb");CHECK(file);CHECK(fwrite(c.text,1,c.length,file)==c.length&&!fclose(file));
    printf("NATIVE_CAPACITY_PATH_GENERATED completeBuiltCheckedReplaySpecializeLowered=1 C11bytes=%zu READindex=0/1 helper=path_capacity GNU=0\n",c.length);
    xr_xir_compile_c_source_free(&c);
}
#endif // XIR_ARRAY_CAPACITY_NATIVE_PATH_PIPELINE_H
