/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_constructor_names_source.h - Authentic initializer name observations
 */
#ifndef XIR_SOURCE_CONSTRUCTOR_NAMES_SOURCE_H
#define XIR_SOURCE_CONSTRUCTOR_NAMES_SOURCE_H
static const char names_generator[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type Context=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64,Array<string>)\n"
    "type OldContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64)\n"
    "fn unsupported()->Generated {\n"
    " const f:Array<FunctionRow>=[];const p:Array<i64>=[];const b:Array<BlockRow>=[]\n"
    " const c:Array<InstructionRow>=[];const l:Array<string>=[];return (0,f,p,b,c,p,l,p)\n}\n"
    "export fn legacy(info:TypeInfo,context:OldContext)->Generated {return unsupported()}\n"
    "export fn generate(info:TypeInfo,context:Context)->Generated {\n"
    " var label=\"none\"\n"
    " if(context.3<0){if(len(context.4)!=0){return unsupported()}}\n"
    " else {if(len(context.4)!=context.1[context.3].4 || len(context.4)!=2){return unsupported()}\n"
    "  label=context.4[0]+\":\"+context.4[1]\n }\n"
    " const method=context.0[0];const functions:Array<FunctionRow>=[(method.0,0,0,method.1,0,1,0,1,0,2,0,0,0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,2,0,0)]\n"
    " const code:Array<InstructionRow>=[(%u,method.1,0,0,0,0,0,0,0,-1),(%u,-1,1,0,0,0,0,0,0,-1)]\n"
    " const operands:Array<i64>=[];const literals:Array<string>=[label];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char names_user[]=
    "import {generate,legacy} from \"./constructor_names\"\n"
    "interface CtorNames {parameterNames()->string}\n"
    "@derive(CtorNames,%s%s)\n"
    "struct Future {private const sum:i64\n"
    " private init(%s:i64,%s:i64){this.sum=%s+%s}\n"
    " static seed()->Future{return Future(0,0)} }\n"
    "export fn run()->string{return Future.seed().parameterNames()}\n"
    "export fn main()->i64{return (run()==\"%s\")?42:0}\n";
#endif // XIR_SOURCE_CONSTRUCTOR_NAMES_SOURCE_H
