/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_json_source.h - Ordinary scalar-record JSON generator
 */
#ifndef XIR_SOURCE_DERIVATION_JSON_SOURCE_H
#define XIR_SOURCE_DERIVATION_JSON_SOURCE_H
static const char json_generator_format[]=
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
    " const integer:i64=%u;const boolean:i64=%u;const stringType:i64=%u\n"
    " var quote:i64=-1;var c:i64=0\n"
    " while(c<len(context.1)){const callee=context.1[c]\n"
    "  if(callee.1==\"jsonQuote\" && callee.4==1 && info.1[callee.5].1==stringType &&\n"
    "    info.1[context.2[callee.3]].1==stringType){if(quote>=0){return unsupported()};quote=callee.2}\n"
    "  c=c+1\n }\n"
    " const method=context.0[0];const result=method.1;const root=info.1[info.0]\n"
    " var code:Array<InstructionRow>=[(opString,result,0,0,0,0,0,0,0,-1)]\n"
    " var literals:Array<string>=[\"{\"];var operands:Array<i64>=[];var previous:i64=1;var i:i64=0\n"
    " while(i<root.7){\n"
    "  const field=info.2[root.6+i];const scalar=info.1[field.1]\n"
    "  if(scalar.0!=0 || (scalar.1!=integer && scalar.1!=boolean && scalar.1!=stringType)){return unsupported()}\n"
    "  const prefix=((i==0)?\"\":\",\")+\"\\\"\"+field.0+\"\\\":\"\n"
    "  const literal=len(literals);literals.push(prefix);const key=len(code)+1\n"
    "  code.push((opString,result,0,0,0,0,literal,0,0,-1))\n"
    "  const value=len(code)+1;code.push((opRead,field.1,0,0,0,0,field.3,0,0,field.3))\n"
    "  const text=len(code)+1\n"
    "  if(scalar.1==stringType){if(quote<0){return unsupported()};const arg=len(operands);operands.push(value)\n"
    "   code.push((opCall,result,arg,1,0,0,-quote-1,0,0,-1))\n"
    "  }else{code.push((opText,result,value,0,0,0,0,0,0,-1))}\n"
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
static const char json_user_source[]=
    "import {generate,jsonQuote} from \"./generator_json\"\n"
    "interface Encode { encodeJSON()->string }\n"
    "@derive(Encode,generate,describe:[\"secret\"],read:[\"left\",\"secret\",\"enabled\",\"label\"])\n"
    "struct Future { private const hidden:string;const left:i64;private const secret:i64;const enabled:bool;const label:string\n"
    " init(hidden:string,left:i64,secret:i64,enabled:bool,label:string){this.hidden=hidden;this.left=left;this.secret=secret;this.enabled=enabled;this.label=label} }\n"
    "export fn encode()->string {return Future(\"never encode\",42,-7,true,\"\\\"\\\\\\n\\t\\0汉😀\").encodeJSON()}\n"
    "export fn main()->i64 {\n"
    " const folded=comptime Future(\"private\",100,-2,false,\"\").encodeJSON()\n"
    " return (folded==\"{\\\"left\\\":100,\\\"secret\\\":-2,\\\"enabled\\\":false,\\\"label\\\":\\\"\\\"}\")?42:0\n}\n";
#endif // XIR_SOURCE_DERIVATION_JSON_SOURCE_H
