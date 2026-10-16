/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_enum_source.h - Ordinary future enum name generation
 */
#ifndef XIR_SOURCE_DERIVATION_ENUM_SOURCE_H
#define XIR_SOURCE_DERIVATION_ENUM_SOURCE_H
static const char enum_generator_format[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type DeriveContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64,Array<string>)\n"
    "fn unsupported()->Generated {\n"
    " const functions:Array<FunctionRow>=[];const parameters:Array<i64>=[];const blocks:Array<BlockRow>=[]\n"
    " const code:Array<InstructionRow>=[];const operands:Array<i64>=[];const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (0,functions,parameters,blocks,code,operands,literals,arguments)\n}\n"
    "export fn generate(info:TypeInfo,context:DeriveContext)->Generated {\n"
    " if(len(context.0)!=1 || context.0[0].3!=0 || context.0[0].4!=0){return unsupported()}\n"
    " const method=context.0[0];const root=info.1[info.0]\n"
    " if(root.4!=%u || root.15==0){return unsupported()}\n"
    " const opTag:i64=%u;const opInteger:i64=%u;const opEqual:i64=%u;const opBranch:i64=%u\n"
    " const opString:i64=%u;const opReturn:i64=%u;const integer:i64=-%u;const boolean:i64=-%u\n"
    " var code:Array<InstructionRow>=[(opTag,integer,0,0,0,0,0,0,0,-1)]\n"
    " var blocks:Array<BlockRow>=[];var literals:Array<string>=[];var i:i64=0\n"
    " while(i<root.15){\n"
    "  const begin=(i==0)?0:len(code);const number=len(code)+1\n"
    "  code.push((opInteger,integer,0,0,0,0,i,0,0,-1))\n"
    "  const equal=len(code)+1;code.push((opEqual,boolean,1,number,0,0,0,0,0,-1))\n"
    "  code.push((opBranch,-1,equal,0,2*i+1,2*i+2,0,0,0,-1))\n"
    "  blocks.push((begin,len(code)-begin,0,0));const success=len(code)\n"
    "  const literal=len(literals);literals.push(info.5[root.14+i].0)\n"
    "  code.push((opString,method.1,0,0,0,0,literal,0,0,-1));const name=len(code)\n"
    "  code.push((opReturn,-1,name,0,0,0,0,0,0,-1));blocks.push((success,2,0,0));i=i+1\n"
    " }\n"
    " const fallback=len(code);const empty=len(literals);literals.push(\"\")\n"
    " code.push((opString,method.1,0,0,0,0,empty,0,0,-1));const name=len(code)\n"
    " code.push((opReturn,-1,name,0,0,0,0,0,0,-1));blocks.push((fallback,2,0,0))\n"
    " const functions:Array<FunctionRow>=[(method.0,0,0,method.1,0,1,0,len(blocks),0,len(code),0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const operands:Array<i64>=[];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char enum_user_source[]=
    "import {generate} from \"./generator_enum\"\n"
    "interface CaseName { caseName()->string }\n"
    "@derive(CaseName,generate)\n"
    "enum Future { Alpha, Payload { value:i64 }, Omega }\n"
    "export fn first()->string {return Future.Alpha.caseName()}\n"
    "export fn payload()->string {return Future.Payload{value:99}.caseName()}\n"
    "export fn last()->string {return Future.Omega.caseName()}\n"
    "export fn main()->i64 {\n"
    " const name=comptime Future.Payload{value:17}.caseName()\n"
    " return (name==\"Payload\")?42:0\n}\n";
#endif // XIR_SOURCE_DERIVATION_ENUM_SOURCE_H
