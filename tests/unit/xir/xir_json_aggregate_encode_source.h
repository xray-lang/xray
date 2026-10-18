/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_aggregate_encode_source.h - Actual aggregate field encoding
 */
#ifndef XIR_JSON_AGGREGATE_ENCODE_SOURCE_H
#define XIR_JSON_AGGREGATE_ENCODE_SOURCE_H
static const char aggregate_generator_format[]=
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
    " const opString:i64=%u;const opRead:i64=%u;const opText:i64=%u;const opConcat:i64=%u;const opReturn:i64=%u;const opCall:i64=%u\n"
    " const integer:i64=%u;const array:i64=%u;const nullable:i64=%u;const stringType:i64=%u\n"
    " var arrays:i64=-1;var optionals:i64=-1;var c:i64=0\n"
    " while(c<len(context.1)){const callee=context.1[c]\n"
    "  if(callee.4==1 && info.1[callee.5].1==stringType){\n"
    "   const argument=info.1[context.2[callee.3]]\n"
    "   if(argument.0==array || argument.0==nullable){const element=info.1[argument.10]\n"
    "    if(element.0==0 && element.1==integer){\n"
    "     if(callee.1==\"jsonIntegerArray\" && argument.0==array){if(arrays>=0){return unsupported()};arrays=callee.2}\n"
    "     if(callee.1==\"jsonOptionalInteger\" && argument.0==nullable){if(optionals>=0){return unsupported()};optionals=callee.2}\n"
    "    }\n   }\n  }\n  c=c+1\n }\n"
    " const method=context.0[0];const result=method.1;const root=info.1[info.0]\n"
    " var code:Array<InstructionRow>=[(opString,result,0,0,0,0,0,0,0,-1)]\n"
    " var literals:Array<string>=[\"{\"];var operands:Array<i64>=[];var previous:i64=1;var i:i64=0\n"
    " while(i<root.7){\n"
    "  const field=info.2[root.6+i];const scalar=info.1[field.1]\n"
    "  if(scalar.0!=array && scalar.0!=nullable){return unsupported()};const element=info.1[scalar.10];if(element.0!=0 || element.1!=integer){return unsupported()}\n"
    "  const prefix=((i==0)?\"\":\",\")+\"\\\"\"+field.0+\"\\\":\"\n"
    "  const literal=len(literals);literals.push(prefix);const key=len(code)+1\n"
    "  code.push((opString,result,0,0,0,0,literal,0,0,-1))\n"
    "  const value=len(code)+1;code.push((opRead,field.1,0,0,0,0,field.3,0,0,field.3))\n"
    "  const text=len(code)+1\n"
    "  const helper=(scalar.0==array)?arrays:optionals;if(helper<0){return unsupported()}\n"
    "  const arg=len(operands);operands.push(value)\n"
    "  code.push((opCall,result,arg,1,0,0,-helper-1,0,0,-1))\n"
    "  const named=len(code)+1;code.push((opConcat,result,previous,key,0,0,0,0,0,-1))\n"
    "  previous=len(code)+1;code.push((opConcat,result,named,text,0,0,0,0,0,-1));i=i+1\n"
    " }\n"
    " const closing=len(literals);literals.push(\"}\");const close=len(code)+1\n"
    " code.push((opString,result,0,0,0,0,closing,0,0,-1))\n"
    " const complete=len(code)+1;code.push((opConcat,result,previous,close,0,0,0,0,0,-1))\n"
    " code.push((opReturn,-1,complete,0,0,0,0,0,0,-1))\n"
    " const functions:Array<FunctionRow>=[(method.0,0,0,result,0,1,0,1,0,len(code),0,len(operands),0,0)]\n"
    " const parameters:Array<i64>=[info.0];const blocks:Array<BlockRow>=[(0,len(code),0,0)]\n"
    " const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char aggregate_user_source[]=
    "import {generate,jsonIntegerArray,jsonOptionalInteger} from \"./json_aggregates\"\n"
    "interface Encode {encodeJSON()->string}\n"
    "@derive(Encode,generate,describe:[\"values\",\"optional\"],read:[\"values\",\"optional\"])\n"
    "struct Future {private const hidden:i64;private const values:Array<i64>;private const optional:i64?\n"
    " init(values:Array<i64>,optional:i64?){this.hidden=99;this.values=values;this.optional=optional} }\n"
    "export fn case0()->string{return Future([],null).encodeJSON()}\n"
    "export fn case1()->string{return Future([1,-2,42],7).encodeJSON()}\n"
    "export fn case2()->string{return Future([(-9223372036854775807-1),9223372036854775807],(-9223372036854775807-1)).encodeJSON()}\n"
    "export fn case3()->string{return Future([0,0,0],0).encodeJSON()}\n"
    "export fn case4()->string{return Future([7],null).encodeJSON()}\n"
    "export fn case5()->string{var values:Array<i64>=[];var i:i64=0;while(i<64){values.push(i-32);i=i+1};return Future(values,9223372036854775807).encodeJSON()}\n"
    "export fn main()->i64{const folded=comptime Future([2,4],null).encodeJSON();return (folded==\"{\\\"values\\\":[2,4],\\\"optional\\\":null}\")?42:0}\n";
#endif // XIR_JSON_AGGREGATE_ENCODE_SOURCE_H
