/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nested_nullable_fixture.h - Complete hand-built three-state Program
 *
 * KEY CONCEPT:
 *   One optional layer is one distinct owned sum, even when its child is optional.
 */
#ifndef XIR_NESTED_NULLABLE_FIXTURE_H
#define XIR_NESTED_NULLABLE_FIXTURE_H
#include "xir/xxir.h"
#define NESTED_INNER ((XrXirType)256)
#define NESTED_OUTER ((XrXirType)257)
static const XrXirTypeNode nested_nodes[] = {
    {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_I64},
    {.kind=XR_XIR_TYPE_NULLABLE,.element=NESTED_INNER}
};
static const XrXirTypes nested_types={nested_nodes,2,NULL,NULL};
static const XrXirInstruction nested_init_ops[]={
    {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
};
static const XrXirInstruction nested_root_ops[]={
    {XR_XIR_CALL,NESTED_OUTER,{0,0},{0},3,{0}},
    {XR_XIR_CALL,NESTED_INNER,{0,1},{0},5,{0}},
    {XR_XIR_NULLABLE_IS_SOME,XR_XIR_BOOL,{1,0},{0},0,{0}},
    {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{3,0},{0},0,{0}}
};
static const XrXirInstruction nested_none_ops[]={
    {XR_XIR_NULLABLE_NONE,NESTED_OUTER,{0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
};
static const XrXirInstruction nested_some_none_ops[]={
    {XR_XIR_NULLABLE_NONE,NESTED_INNER,{0},{0},0,{0}},
    {XR_XIR_NULLABLE_SOME,NESTED_OUTER,{0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{1,0},{0},0,{0}}
};
static const XrXirInstruction nested_some7_ops[]={
    {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
    {XR_XIR_NULLABLE_SOME,NESTED_INNER,{0},{0},0,{0}},
    {XR_XIR_NULLABLE_SOME,NESTED_OUTER,{1,0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{2,0},{0},0,{0}}
};
static const XrXirInstruction nested_unwrap_outer_ops[]={
    {XR_XIR_NULLABLE_UNWRAP,NESTED_INNER,{0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{1,0},{0},0,{0}}
};
static const XrXirInstruction nested_unwrap_inner_ops[]={
    {XR_XIR_NULLABLE_UNWRAP,XR_XIR_I64,{0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{1,0},{0},0,{0}}
};
static const XrXirInstruction nested_presence_ops[]={
    {XR_XIR_NULLABLE_IS_SOME,XR_XIR_BOOL,{0},{0},0,{0}},
    {XR_XIR_RETURN,XR_XIR_UNIT,{1,0},{0},0,{0}}
};
static const XrXirBlock nested_blocks[]={
    {0,1,0,0},{0,5,0,0},{0,2,0,0},{0,3,0,0},
    {0,4,0,0},{0,2,0,0},{0,2,0,0},{0,2,0,0}
};
static const XrXirType nested_outer_parameters[]={NESTED_OUTER};
static const XrXirType nested_inner_parameters[]={NESTED_INNER};
static const uint32_t nested_root_operands[]={0};
static const XrXirFunction nested_functions[]={
    {"$init",5,NULL,0,XR_XIR_UNIT,nested_blocks+0,1,nested_init_ops,1,NULL,0},
    {"root",4,NULL,0,XR_XIR_I64,nested_blocks+1,1,nested_root_ops,5,nested_root_operands,1},
    {"none",4,NULL,0,NESTED_OUTER,nested_blocks+2,1,nested_none_ops,2,NULL,0},
    {"some_none",9,NULL,0,NESTED_OUTER,nested_blocks+3,1,nested_some_none_ops,3,NULL,0},
    {"some7",5,NULL,0,NESTED_OUTER,nested_blocks+4,1,nested_some7_ops,4,NULL,0},
    {"unwrap_outer",12,nested_outer_parameters,1,NESTED_INNER,nested_blocks+5,1,nested_unwrap_outer_ops,2,NULL,0},
    {"unwrap_inner",12,nested_inner_parameters,1,XR_XIR_I64,nested_blocks+6,1,nested_unwrap_inner_ops,2,NULL,0},
    {"presence",8,nested_outer_parameters,1,XR_XIR_BOOL,nested_blocks+7,1,nested_presence_ops,2,NULL,0}
};
static const XrXirSourceModule nested_modules[]={{"nested",6,NULL,0,0}};
static const XrXirFunctionIdentity nested_identities[]={
    {0},{.exported=1},{.exported=1},{.exported=1},
    {.exported=1},{.exported=1},{.exported=1},{.exported=1}
};
static const XrXirDeclarations nested_declarations={nested_modules,1,nested_identities,NULL,0,NULL,0,0,1,NULL};
static const XrXirModule nested_built={XR_XIR_BUILT,nested_functions,8,&nested_declarations,NULL,
    &nested_types,NULL,XR_XIR_PROGRAM,NULL};
#endif // XIR_NESTED_NULLABLE_FIXTURE_H
