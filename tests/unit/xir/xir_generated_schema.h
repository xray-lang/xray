/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generated_data_source.h - Ordinary generator tables from future facts
 */
#ifndef XIR_GENERATED_SCHEMA_H
#define XIR_GENERATED_SCHEMA_H
static const char generated_schema_source[]=
    "type FieldInfo=(string,i64,i64,i64)\n"
    "type ParameterInfo=(i64,i64)\n"
    "type VariantInfo=(string,i64,i64)\n"
    "type NodeInfo=(i64,i64,string,string,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64)\n"
    "type TypeInfo=(i64,Array<NodeInfo>,Array<FieldInfo>,Array<i64>,Array<ParameterInfo>,Array<VariantInfo>)\n"
    "type FunctionRow=(string,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64)\n"
    "type BlockRow=(i64,i64,i64,i64)\n"
    "type InstructionRow=(i64,i64,i64,i64,i64,i64,i64,i64,i64,i64)\n"
    "type Generated=(i64,Array<FunctionRow>,Array<i64>,Array<BlockRow>,Array<InstructionRow>,Array<i64>,Array<string>,Array<i64>)\n";
#endif // XIR_GENERATED_SCHEMA_H
