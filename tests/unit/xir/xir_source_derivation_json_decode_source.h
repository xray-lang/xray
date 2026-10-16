/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_json_decode_source.h - Generated nullable initializer calls
 */
#ifndef XIR_SOURCE_DERIVATION_JSON_DECODE_SOURCE_H
#define XIR_SOURCE_DERIVATION_JSON_DECODE_SOURCE_H
static const char json_decode_generator[]=
    "%s"
    "type MethodInfo=(string,i64,i64,i64,i64)\n"
    "type CalleeInfo=(string,string,i64,i64,i64,i64)\n"
    "type DeriveContext=(Array<MethodInfo>,Array<CalleeInfo>,Array<i64>,i64)\n"
    "fn unsupported()->Generated {\n"
    " const f:Array<FunctionRow>=[];const p:Array<i64>=[];const b:Array<BlockRow>=[]\n"
    " const c:Array<InstructionRow>=[];const l:Array<string>=[];return (0,f,p,b,c,p,l,p)\n}\n"
    "export fn generate(info:TypeInfo,context:DeriveContext)->Generated {\n"
    " if(context.3<0 || len(context.0)!=1){return unsupported()}\n"
    " const method=context.0[0];const ctor=context.1[context.3]\n"
    " const integer:i64=%u;const text:i64=%u;const nullable:i64=%u\n"
    " if(method.3!=1 || method.4!=0 || ctor.4!=1 ||\n"
    "  info.1[context.2[ctor.3]].1!=integer || info.1[context.2[method.2]].1!=text || info.1[method.1].0!=nullable){return unsupported()}\n"
    " var parse:i64=-1;var parsedType:i64=-1;var i:i64=0\n"
    " while(i<len(context.1)){const callee=context.1[i];const result=info.1[callee.5]\n"
    "  if(callee.1==\"jsonInteger\" && callee.4==1 && info.1[context.2[callee.3]].1==text &&\n"
    "   result.0==nullable && info.1[result.10].1==integer){\n"
    "   if(parse>=0){return unsupported()};parse=callee.2;parsedType=callee.5\n"
    "  };i=i+1\n }\n"
    " if(parse<0){return unsupported()}\n"
    " const call:i64=%u;const some:i64=%u;const branch:i64=%u;const unwrap:i64=%u\n"
    " const wrap:i64=%u;const none:i64=%u;const ret:i64=%u;const boolType:i64=-%u;const intType:i64=-%u\n"
    " const code:Array<InstructionRow>=[\n"
    "  (call,parsedType,0,1,0,0,-parse-1,0,0,-1),(some,boolType,2,0,0,0,0,0,0,-1),\n"
    "  (branch,-1,3,0,1,2,0,0,0,-1),(unwrap,intType,2,0,0,0,0,0,0,-1),\n"
    "  (call,info.0,1,1,0,0,-ctor.2-1,0,0,-1),(wrap,method.1,6,0,0,0,0,0,0,-1),\n"
    "  (ret,-1,7,0,0,0,0,0,0,-1),(none,method.1,0,0,0,0,0,0,0,-1),(ret,-1,9,0,0,0,0,0,0,-1)]\n"
    " const functions:Array<FunctionRow>=[(method.0,0,0,method.1,0,2,0,3,0,9,0,2,0,0)]\n"
    " const parameters:Array<i64>=[info.0,context.2[method.2]]\n"
    " const blocks:Array<BlockRow>=[(0,3,0,0),(3,4,0,0),(7,2,0,0)];const operands:Array<i64>=[1,5]\n"
    " const literals:Array<string>=[];const arguments:Array<i64>=[]\n"
    " return (1,functions,parameters,blocks,code,operands,literals,arguments)\n}\n";
static const char json_decode_user[]=
    "import {generate,jsonInteger} from \"./generator_json_decode\"\n"
    "interface DecodeInteger<T> {decodeJSON(text:string)->T?}\n"
    "@derive(DecodeInteger<Future>,generate%s)\n"
    "struct Future {private const amount:i64\n"
    " private init(value:i64){this.amount=(value<0)?0:value}\n"
    " static seed(value:i64)->Future{return Future(value)}\n"
    " observed()->i64{return this.amount} }\n"
    "export fn decode(text:string)->i64? {const value=Future.seed(19).decodeJSON(text)\n"
    " if(value==null){return null};return value!.observed()}\n"
    "export fn main()->i64 {const folded=comptime decode(\"-7\");return ((folded ?? 999)==0)?42:0}\n";
#endif // XIR_SOURCE_DERIVATION_JSON_DECODE_SOURCE_H
