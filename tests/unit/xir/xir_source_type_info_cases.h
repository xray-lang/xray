/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_type_info_cases.h - Ordinary language consumption of owning facts
 */
#ifndef XIR_SOURCE_TYPE_INFO_CASES_H
#define XIR_SOURCE_TYPE_INFO_CASES_H
static const char source_type_info_schema_text[]=
    "type FieldInfo=(string,i64,i64,i64)\n"
    "type ParameterInfo=(i64,i64)\n"
    "type VariantInfo=(string,i64,i64)\n"
    "type NodeInfo=(i64,i64,string,string,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64,i64)\n"
    "type TypeInfo=(i64,Array<NodeInfo>,Array<FieldInfo>,Array<i64>,Array<ParameterInfo>,Array<VariantInfo>)\n"
    "struct Record { private const secret:i64; const label:string }\n"
    "fn inspect(info:TypeInfo)->i64 {\n"
    " const root=info.1[info.0]; const fields=info.2\n"
    " if(root.0!=4 || root.1!= -1 || root.3!=\"Record\" || len(info.1)!=3 || len(fields)!=2 || root.6!=0 || root.7!=2){return 0}\n"
    " if(fields[0].0!=\"secret\" || fields[0].2!=1 || fields[0].3!=0 || fields[1].0!=\"label\" || fields[1].2!=0 || fields[1].3!=1){return 1}\n"
    " const a=info.1[fields[0].1]; const b=info.1[fields[1].1]\n"
    " if(a.0!=0 || a.3!=\"i64\" || b.0!=0 || b.3!=\"string\" || a.7!=0 || b.7!=0){return 2}\n"
    " if(root.9!=0 || root.10!= -1 || root.12!=0 || root.13!= -1 || root.15!=0 || len(info.3)!=0 || len(info.4)!=0 || len(info.5)!=0){return 3}\n"
    " return 42\n}\n"
    "export fn run()->i64 { return comptime inspect(typeInfo<Record>()) }\n";
static void source_type_info_semantics(void) {
    source_comptime_test_answer(source_type_info_schema_text,XR_XIR_I64,42);
    char receiving[4096];
    const char *entry=strstr(source_type_info_schema_text,"export fn run");CHECK(entry);
    size_t prefix=(size_t)(entry-source_type_info_schema_text);
    CHECK(prefix<sizeof(receiving));memcpy(receiving,source_type_info_schema_text,prefix);
    const char *consumer="export fn run()->i64 { const info=comptime typeInfo<Record>(); return inspect(info) }\n";
    CHECK(strlen(consumer)<sizeof(receiving)-prefix);memcpy(receiving+prefix,consumer,strlen(consumer)+1);
    source_comptime_test_result(receiving,XR_XIR_I64,42,false);
    source_comptime_test_answer("export fn run()->i64 { return comptime { const info=typeInfo<Array<i64>>(); const a=info.1[0]; const e=info.1[a.10]; (a.0==2 && e.3==\"i64\" && len(info.1)==2)?42:0 } }\n",XR_XIR_I64,42);
    source_comptime_test_answer("struct Pair<A,B>{const first:A;const second:B}\nexport fn run()->i64 {return comptime {const info=typeInfo<Pair<string,i64>>();const root=info.1[0];const a=info.3[root.8];const b=info.3[root.8+1];(root.9==2 && info.1[a].3==\"string\" && info.1[b].3==\"i64\" && info.2[0].1==a && info.2[1].1==b)?42:0}}\n",XR_XIR_I64,42);
    source_comptime_test_answer("enum Choice {Empty,Some{value:i64},Text{value:string}}\nexport fn run()->i64 {return comptime {const info=typeInfo<Choice>();const root=info.1[0];const variants=info.5;(root.15==3 && len(variants)==3 && variants[0].0==\"Empty\" && variants[0].2==0 && variants[1].0==\"Some\" && variants[1].1==0 && variants[1].2==1 && variants[2].0==\"Text\" && variants[2].1==1 && variants[2].2==1 && info.2[1].3==1)?42:0}}\n",XR_XIR_I64,42);
    source_comptime_test_answer("type Action=fn(i64,string)->bool\nexport fn run()->i64 {return comptime {const info=typeInfo<Action>();const root=info.1[0];const args=info.4;(root.0==1 && root.12==2 && len(args)==2 && args[0].1==0 && args[1].1==0 && info.1[args[0].0].3==\"i64\" && info.1[args[1].0].3==\"string\" && info.1[root.13].3==\"bool\")?42:0}}\n",XR_XIR_I64,42);
    source_comptime_test_answer("class Link{const next:Link?;init(next:Link?){this.next=next}}\nexport fn run()->i64 {return comptime {const info=typeInfo<Link>();const root=info.1[0];const edge=info.1[info.2[root.6].1];(len(info.1)==2 && edge.0==5 && edge.10==0)?42:0}}\n",XR_XIR_I64,42);
    source_comptime_test_refuse("struct Link{const next:Link?}\nexport fn run()->i64 {return comptime typeInfo<Link>().0}\n",XR_XIR_BAD_TYPE);
    source_comptime_test_answer("fn typeInfo<T>()->i64{return 42}\nexport fn run()->i64 {return comptime typeInfo<i64>()}\n",XR_XIR_I64,42);
    source_comptime_test_refuse("fn inspect<T>()->i64 {return typeInfo<T>().0}\nexport fn run()->i64{return 1}\n",XR_XIR_BAD_TYPE);
    source_comptime_test_refuse("export fn run()->i64 {return typeInfo<i64>(1).0}\n",XR_XIR_BAD_TYPE);
}
#endif
