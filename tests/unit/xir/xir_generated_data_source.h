/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generated_data_source.h - Ordinary generator tables from future facts
 */
#ifndef XIR_GENERATED_DATA_SOURCE_H
#define XIR_GENERATED_DATA_SOURCE_H
#include "xir_generated_schema.h"
static const char generated_library_format[]=
    "%s"
    "fn privateBody(info:TypeInfo,field:FieldInfo,mode:i64)->Generated {\n"
    " const result=(mode==3)?9999:field.1\n"
    " const root=(mode==4)?1:0\n"
    " const functions:Array<FunctionRow>=[(\"measure\",0,root,result,0,1,0,1,0,(mode==2)?3:2,0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0]\n"
    " const blocks:Array<BlockRow>=[(0,(mode==2)?3:2,0,0)]\n"
    " var code:Array<InstructionRow>=[(%u,(mode==1)?%d:field.1,0,0,0,0,field.3,0,0,field.3),(%u,-1,1,0,0,0,0,0,0,-1)]\n"
    " if(mode==2){code=[(%u,field.1,0,0,0,0,field.3,0,0,field.3),(%u,-1,0,1,0,0,field.3,0,0,field.3),(%u,-1,1,0,0,0,0,0,0,-1)]}\n"
    " const operands:Array<i64>=[]; const literals:Array<string>=[]; const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n"
    "export fn generate(info:TypeInfo,mode:i64)->Generated {\n"
    " const root=info.1[info.0];var i:i64=0\n"
    " while(i<root.7){const field=info.2[root.6+i];if(info.1[field.1].3==\"i64\"){return privateBody(info,field,mode)};i=i+1}\n"
    " return privateBody(info,info.2[0],mode)\n}\n";
static const char generated_user_format[]=
    "import {generate} from \"./generator\"\n"
    "%s"
    "interface Measure { measure()->i64 }\n"
    "struct Future { private const hidden:string; private const secret:i64\n"
    " init(hidden:string,secret:i64){this.hidden=hidden;this.secret=secret} }\n"
    "export fn make()->Future {return Future(\"unused\",42)}\n"
    "export fn produce(mode:i64)->Generated {return generate(typeInfo<Future>(),mode)}\n"
    "export fn main()->i64 {return 41}\n";
#endif // XIR_GENERATED_DATA_SOURCE_H
