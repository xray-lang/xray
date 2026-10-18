/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_zero_scratch_fixture.h - Zero-argument cleanup through ordinary admission
 */
#ifndef XIR_ZERO_SCRATCH_FIXTURE_H
#define XIR_ZERO_SCRATCH_FIXTURE_H
#include "xir_construction_fixture.h"
static XrXirArtifact *zero_scratch_cleanup_lowered(const XrXirCompileContext *context) {
    const XrXirInstruction done={XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}};
    const XrXirInstruction cleanup[]={
        {XR_XIR_VIEW_CLEANUP,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirInstruction code[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_CLEANUP_REGISTER,XR_XIR_UNIT,{0,0},{1},2,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    const XrXirBlock one={0,1,0,0},blocks[]={{0,2,0,0},{2,1,0,2}};
    const XrXirBlock cleanup_block={0,2,0,0};
    const XrXirFunction functions[]={
        {"init",4,NULL,0,XR_XIR_UNIT,&one,1,&done,1,NULL,0},
        {"main",4,NULL,0,XR_XIR_I64,blocks,2,code,3,NULL,0},
        {"cleanup",7,NULL,0,XR_XIR_UNIT,&cleanup_block,1,cleanup,2,NULL,0}};
    XrXirFunctionIdentity identities[3]={{0}};identities[2].cleanup_owner=2;
    const XrXirSourceModule source={.name="root",.name_length=4,.initializer=0};
    const XrXirDeclarations declarations={&source,1,identities,NULL,0,NULL,0,0,1,NULL};
    const XrXirModule module={XR_XIR_BUILT,functions,3,&declarations,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    XrXirArtifact *checked=NULL,*lowered=NULL;
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xir_fixture_check(context,&module,&checked,&diagnostic);
    if (status!=XR_XIR_OK) {
        XrXirEffects *effects=NULL;
        XrXirStatus inferred=xr_xir_compile_effects_infer_verified(context,&module,&effects);
        XrXirStatus matched=inferred==XR_XIR_OK?xir_effects_parameters_match(context,&module,effects):inferred;
        fprintf(stderr,"zero cleanup status=%u f=%u b=%u i=%u inferred=%u matched=%u\n",
            status,diagnostic.function,diagnostic.block,diagnostic.instruction,inferred,matched);
        xr_xir_compile_effects_free(effects);
    }
    CHECK(status==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_layout(lowered,1)->outgoing_count==0);
    CHECK(xr_xir_compile_artifact_layout(lowered,2)->outgoing_count==0);return lowered;
}
#endif // XIR_ZERO_SCRATCH_FIXTURE_H
