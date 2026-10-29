/* Complete nullable programs retain independent output and refusal oracles. */
#ifndef HIR_NULLABLE_FIXTURE_H
#define HIR_NULLABLE_FIXTURE_H
#include "xray_source.h"
static const char nullable_operators_source[]=
    "// Presence tests, force unwrap and \077\077 look only at the outermost nullable layer, so a class element\r\n"
    "// needs no equality, and \077\077 evaluates its default only when the left side is absent.\r\n"
    "class Node {\r\n"
    "    v: i64\r\n"
    "    init(v: i64) { this.v = v }\r\n"
    "}\r\n"
    "fn pick(a: i64\077, b: i64\077) -> i64 {\r\n"
    "    return (a \077\077 100) + (b \077\077 1)\r\n"
    "}\r\n"
    "fn name(s: string\077) -> string {\r\n"
    "    return s \077\077 \"none\"\r\n"
    "}\r\n"
    "var calls = 0\r\n"
    "fn fallback() -> i64 {\r\n"
    "    calls = calls + 1\r\n"
    "    return 7\r\n"
    "}\r\n"
    "fn main() {\r\n"
    "    var n: i64\077 = 5\r\n"
    "    var m: i64\077 = null\r\n"
    "    print(pick(n, m))\r\n"
    "    print(pick(null, 7))\r\n"
    "    print(n != null)\r\n"
    "    print(m == null)\r\n"
    "    print(n == null)\r\n"
    "    print(n!)\r\n"
    "    print(name(\"abc\"))\r\n"
    "    print(name(null))\r\n"
    "    var c: Node\077 = Node(9)\r\n"
    "    var d: Node\077 = null\r\n"
    "    print(c != null)\r\n"
    "    print(d == null)\r\n"
    "    print(c!.v)\r\n"
    "    var e: i64\077 = n \077\077 m\r\n"
    "    print(e == 5)\r\n"
    "    var f: i64\077 = m \077\077 null\r\n"
    "    print(f == null)\r\n"
    "    print((n \077\077 fallback()) + (m \077\077 fallback()))\r\n"
    "    print(calls)\r\n"
    "}\r\n"
    "main()\r\n";
static const char nullable_operators_expected[]=
    "6\n"
    "107\n"
    "true\n"
    "true\n"
    "false\n"
    "5\n"
    "abc\n"
    "none\n"
    "true\n"
    "true\n"
    "9\n"
    "true\n"
    "true\n"
    "12\n"
    "1\n";
static const char nullable_contract_factory_lazy_trace_source[]=
    "fn layer<T>(value: T) -> T\077 { return value }\r\n"
    "fn none<T>() -> T\077 { return null }\r\n"
    "// Tags, exact context and lazy traces are independent golden facts.\r\n"
    "var trace = 0\r\n"
    "fn factory(mode: i64) -> i64\077\077 {\r\n"
    "    trace = trace + 1\r\n"
    "    if (mode == 0) { return none<i64\077>() }\r\n"
    "    if (mode == 1) { return layer<i64\077>(none<i64>()) }\r\n"
    "    return layer<i64\077>(layer<i64>(7))\r\n"
    "}\r\n"
    "fn fallback() -> i64\077 {\r\n"
    "    trace = trace + 100\r\n"
    "    return layer<i64>(9)\r\n"
    "}\r\n"
    "fn show(mode: i64) {\r\n"
    "    trace = 0\r\n"
    "    const result = factory(mode) \077\077 fallback()\r\n"
    "    print(result \077\077 -1)\r\n"
    "    print(trace)\r\n"
    "    trace = 0\r\n"
    "    print((factory(mode) \077\077 none<i64>()) \077\077 9)\r\n"
    "    print(trace)\r\n"
    "}\r\n"
    "fn innerAbsent(value: i64\077) -> bool { return value == null }\r\n"
    "fn outerAbsent(value: i64\077\077) -> bool { return value == null }\r\n"
    "show(0)\r\n"
    "show(1)\r\n"
    "show(2)\r\n"
    "trace = 0\r\n"
    "const uncontexted = factory(1) \077\077 null\r\n"
    "print(innerAbsent(uncontexted))\r\n"
    "print(trace)\r\n"
    "trace = 0\r\n"
    "const outer: i64\077\077 = factory(1) \077\077 null\r\n"
    "print(outerAbsent(outer))\r\n"
    "print(trace)\r\n";
static const char nullable_contract_factory_lazy_trace_expected[]=
    "9\n"
    "101\n"
    "9\n"
    "1\n"
    "-1\n"
    "1\n"
    "9\n"
    "1\n"
    "7\n"
    "1\n"
    "7\n"
    "1\n"
    "true\n"
    "1\n"
    "false\n"
    "1\n";
static const char complete_source[]=
    "fn layer<T>(value:T)->T\077{return value}\n"
    "fn none<T>()->T\077{return null}\n"
    "var visits=0\n"
    "fn fallback()->i64{visits=visits+100;return 9}\n"
    "fn once(value:i64)->i64{visits=visits+1;return value}\n"
    "fn take(value:i64\077)->i64{return value\077\077fallback()}\n"
    "print(once(7)!,once(8)\077\077fallback(),visits)\n"
    "print(take(3),take(null),visits)\n"
    "var some:i64\077=5\n"
    "var absent:i64\077=null\n"
    "print(some==(null),(null)==absent,(null)!=some)\n"
    "fn reset(value:ref i64\077)->i64{value=null;return 9}\n"
    "fn guarded(value:i64\077)->i64{\n"
    "    var saved=value\n"
    "    if(saved!=null){const held=saved\077\077reset(ref saved);return saved+held}\n"
    "    return 0\n"
    "}\n"
    "print(guarded(7),guarded(null))\n"
    "fn clear(value:ref i64\077)->bool{value=null;return true}\n"
    "fn invalidated(value:i64\077)->i64{\n"
    "    var saved=value\n"
    "    if(saved!=null&&clear(ref saved)){return saved\077\077""9}\n"
    "    return 0\n"
    "}\n"
    "print(invalidated(7))\n"
    "fn triple(value:i64\077\077\077)->i64{\n"
    "    if(value!=null&&value!=null){\n"
    "        print(value! == null,value!! == null,(value!)! == null)\n"
    "        return 14\n"
    "    }\n"
    "    return -1\n"
    "}\n"
    "print(triple(layer<i64\077\077>(layer<i64\077>(none<i64>()))))\n"
    "var text:string\077=\"a\\0\344\270\255\360\237\230\200\"\n"
    "print(text!,text\077\077\"unused\")\n"
    "print(none<string>()\077\077\"none\")\n"
    "var flag:bool\077=false\n"
    "print(flag!=null,flag!)\n"
    "var number:f64\077=-0.0\n"
    "print(number!=null,(number\077\077""1.0)==0.0)\n"
    "const callback=layer(fn()->i64{return 7})\n"
    "print(callback!=null,(callback!)())\n";
static const char complete_expected[]=
    "7 8 2\n"
    "3 9 102\n"
    "false true true\n"
    "14 0\n"
    "9\n"
    "true true true\n"
    "14\n"
    "a\000\344\270\255\360\237\230\200 a\000\344\270\255\360\237\230\200\n"
    "none\n"
    "true false\n"
    "true true\n"
    "true 7\n";
typedef struct NullableFixture {const char *name,*source,*expected;size_t length,expected_length;} NullableFixture;
static const NullableFixture nullable_fixtures[]={
    {"nullable_operators",nullable_operators_source,nullable_operators_expected,sizeof(nullable_operators_source)-1,sizeof(nullable_operators_expected)-1},
    {"nullable_contract_factory_lazy_trace",nullable_contract_factory_lazy_trace_source,nullable_contract_factory_lazy_trace_expected,sizeof(nullable_contract_factory_lazy_trace_source)-1,sizeof(nullable_contract_factory_lazy_trace_expected)-1},
    {"complete",complete_source,complete_expected,sizeof(complete_source)-1,sizeof(complete_expected)-1},
};
typedef struct NullableRefusal {const char *name,*source,*message;XrSourceStatus status;int line,column;} NullableRefusal;
static const NullableRefusal nullable_refusals[]={
    {"uncontexted_null","var value=null\n","null requires an exact nullable context",XR_SOURCE_REJECTED,1,11},
    {"nonnull_equal","fn f(value:i64)->bool{return value==null}\n","comparison with null requires a nullable operand",XR_SOURCE_REJECTED,1,35},
    {"nonnull_unequal_left","fn f(value:bool)->bool{return null!=value}\n","comparison with null requires a nullable operand",XR_SOURCE_REJECTED,1,35},
    {"two_nulls","print(null==null)\n","null requires an exact nullable context",XR_SOURCE_REJECTED,1,7},
    {"wrong_default","fn f(value:i64\077)->i64{return value\077\077\"bad\"}\n","expression cannot satisfy its declared type",XR_SOURCE_REJECTED,1,37},
    {"dead_default","fn f(value:i64)->i64{return value\077\077\"bad\"}\n","expression cannot satisfy its declared type",XR_SOURCE_REJECTED,1,36},
    {"claimed_default","fn f(value:i64\077)->i64{if(value!=null){return value\077\077\"bad\"};return 0}\n","expression cannot satisfy its declared type",XR_SOURCE_REJECTED,1,53},
    {"nullable_default","fn f(value:i64\077)->i64\077{return value\077\077\"bad\"}\n","expression cannot satisfy its declared type",XR_SOURCE_REJECTED,1,38},
    {"default_signedness","fn f(value:i64\077,other:u64)->i64{return value\077\077other}\n","expression cannot satisfy its declared type",XR_SOURCE_REJECTED,1,47},
    {"null_ordering","fn f(value:i64\077)->bool{return value<null}\n","operator requires a declared concrete operand contract",XR_SOURCE_REJECTED,1,0},
    {"nullable_truth","fn f(value:bool\077){if(value){}}\n","if requires bool",XR_SOURCE_REJECTED,1,19},
    {"assignment_invalidates","fn f(value:i64\077)->i64{var saved=value;if(saved!=null){saved=null;return saved+1};return 0}\n","operator requires a declared concrete operand contract",XR_SOURCE_REJECTED,1,78},
};
typedef struct NullablePanic {const char *name,*source,*expected;size_t expected_length;} NullablePanic;
static const char nullable_unwrap_panic_source[]=
    "fn main() {\r\n"
    "    var present: i64\077 = 3\r\n"
    "    var absent: i64\077 = null\r\n"
    "    print(present!)\r\n"
    "    print(absent!)\r\n"
    "    print(present!)\r\n"
    "}\r\n"
    "main()\r\n";
static const char nullable_unwrap_panic_expected[]=
    "3\n";
static const char nullable_contract_triple_third_fault_source[]=
    "fn layer<T>(value: T) -> T\077 { return value }\r\n"
    "fn none<T>() -> T\077 { return null }\r\n"
    "// The third explicit operation consumes the remaining inner None.\r\n"
    "fn probe(x: i64\077\077\077) -> i64 {\r\n"
    "    if (x != null && x != null) {\r\n"
    "        print(x! == null)\r\n"
    "        print(x!! == null)\r\n"
    "        return x!!!\r\n"
    "    }\r\n"
    "    return 0\r\n"
    "}\r\n"
    "print(probe(layer<i64\077\077>(layer<i64\077>(none<i64>()))))\r\n";
static const char nullable_contract_triple_third_fault_expected[]=
    "true\n"
    "true\n";
static const NullablePanic nullable_panics[]={
    {"nullable_unwrap_panic",nullable_unwrap_panic_source,nullable_unwrap_panic_expected,sizeof(nullable_unwrap_panic_expected)-1},
    {"nullable_contract_triple_third_fault",nullable_contract_triple_third_fault_source,nullable_contract_triple_third_fault_expected,sizeof(nullable_contract_triple_third_fault_expected)-1},
};
#endif // HIR_NULLABLE_FIXTURE_H
