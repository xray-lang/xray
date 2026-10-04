/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * KEY CONCEPT:
 *   Fixed Unicode scalar expectations exercise the canonical finite pipeline.
 */
#ifndef XIR_RUNE_LEAF_FIXTURE_H
#define XIR_RUNE_LEAF_FIXTURE_H
static XrXirStatus rune_leaf_build(const XrXirCompileContext *ctx,XrXirArtifact **output) {
    XrXirType integer=XR_XIR_I64,rune=XR_XIR_RUNE;
    XrXirInstruction first[]={
        {XR_XIR_INTEGER_TO_RUNE,XR_XIR_RUNE,{0},{0},0,{0}},
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_I64,{1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    XrXirInstruction point[]={
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction unsigned_point[]={
        {XR_XIR_RUNE_TO_INTEGER,XR_XIR_U32,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{1},{0},0,{0}}};
    XrXirInstruction literal[]={
        {XR_XIR_CONST_RUNE,XR_XIR_RUNE,{0},{0},0x1f600,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock blocks[]={{0,3,0,0},{0,2,0,0},{0,2,0,0},{0,2,0,0}};
    XrXirFunction functions[]={
        {"roundtrip",9,&integer,1,XR_XIR_I64,&blocks[0],1,first,3,NULL,0},
        {"point",5,&rune,1,XR_XIR_I64,&blocks[1],1,point,2,NULL,0},
        {"unsigned",8,&rune,1,XR_XIR_U32,&blocks[2],1,unsigned_point,2,NULL,0},
        {"literal",7,NULL,0,XR_XIR_RUNE,&blocks[3],1,literal,2,NULL,0}};
    XrXirModule module={XR_XIR_BUILT,functions,4,NULL,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirArtifact *checked=NULL;
    XrXirStatus status=xr_xir_compile_check(ctx,&module,&checked,NULL);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(checked,&target,output,NULL);
    xr_xir_compile_artifact_free(checked);return status;
}
#endif // XIR_RUNE_LEAF_FIXTURE_H
