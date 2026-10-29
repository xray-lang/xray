/* Boolean qualification, short-circuit effects and existing nullable facts. */
#ifndef HIR_BOOLEAN_FIXTURE_H
#define HIR_BOOLEAN_FIXTURE_H
#include "xray_source.h"
static const char boolean_source[] =
    "var calls=0\n"
    "fn tick(value:bool)->bool{calls=calls+1;return value}\n"
    "print(!false,!true,!!true,!!!true)\n"
    "print(false&&tick(true),true||tick(false),calls)\n"
    "print(true&&tick(false),false||tick(true),calls)\n"
    "print(tick(false)&&tick(true),tick(true)||tick(false),calls)\n"
    "var score=0\n"
    "if(!false&&true){score=11}else{score=99}\n"
    "var n=0\n"
    "while(n<3&&tick(true)){n=n+1}\n"
    "for(var i=0;i<2||false;i++){score=score+1}\n"
    "for(var i=0;;i++){break}\n"
    "var selected=tick(true)?score:99\n"
    "print(selected,n,calls)\n"
    "fn guarded(x:i64?)->i64{if(!(x==null)&&x>0){return x};return 0}\n"
    "fn alternative(x:i64?)->bool{return x==null||x>0}\n"
    "fn through(x:i64?)->i64{return x!=null?x:9}\n"
    "print(guarded(5),guarded(null),alternative(null),alternative(6),through(null),through(7))\n"
    "fn nullableLoops(){\n"
    "var cur:i64?=3\n"
    "var sum=0\n"
    "while(cur!=null){sum=sum+cur;cur=null}\n"
    "print(sum)\n"
    "var text:i64?=2\n"
    "var total=0\n"
    "for(var i=0;i<3&&text!=null;i++){total=total+text}\n"
    "print(total)\n"
    "}\n"
    "nullableLoops()\n";
static const char boolean_expected[] =
    "true false true false\n"
    "false true 0\n"
    "false true 2\n"
    "false true 4\n"
    "13 3 8\n"
    "5 0 true true 9 7\n"
    "3\n"
    "6\n";
typedef struct BooleanRefusal {const char *name,*source,*message;XrSourceStatus status;} BooleanRefusal;
static const BooleanRefusal boolean_refusals[] = {
    {"not_integer","fn f(a:i64)->bool{return !a}\n","logical operand must be bool",XR_SOURCE_REJECTED},
    {"and_left","fn f(a:i64,b:bool)->bool{return a&&b}\n","logical operand must be bool",XR_SOURCE_REJECTED},
    {"and_right","fn f(a:bool,b:i64)->bool{return a&&b}\n","logical operand has no condition flow",XR_SOURCE_BAD_ARGUMENT},
    {"or_left","fn f(a:i64,b:bool)->bool{return a||b}\n","logical operand must be bool",XR_SOURCE_REJECTED},
    {"or_right","fn f(a:bool,b:i64)->bool{return a||b}\n","logical operand has no condition flow",XR_SOURCE_BAD_ARGUMENT},
    {"and_skipped","var answer=false&&1\n","logical operand has no condition flow",XR_SOURCE_BAD_ARGUMENT},
    {"or_skipped","var answer=true||1\n","logical operand has no condition flow",XR_SOURCE_BAD_ARGUMENT},
    {"if_integer","fn f(a:i64){if(a){}}\n","if requires bool",XR_SOURCE_REJECTED},
    {"while_integer","fn f(a:i64){while(a){break}}\n","loop condition requires bool",XR_SOURCE_REJECTED},
    {"for_integer","fn f(a:i64){for(var i=0;a;i++){break}}\n","loop condition requires bool",XR_SOURCE_REJECTED},
    {"conditional_integer","fn f(a:i64)->i64{return a?1:2}\n","conditional requires bool",XR_SOURCE_REJECTED},
    {"not_nullable","fn f(a:bool?)->bool{return !a}\n","logical operand must be bool",XR_SOURCE_REJECTED},
    {"if_nullable","fn f(a:bool?){if(a){}}\n","if requires bool",XR_SOURCE_REJECTED},
    {"write_invalidates","fn f(a:i64?)->i64{var x=a;if(x!=null){x=null;return x+1};return 0}\n","operator requires a declared concrete operand contract",XR_SOURCE_REJECTED}
};
#endif // HIR_BOOLEAN_FIXTURE_H
