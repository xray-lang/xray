/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef ARRAY_BINDING_FIXTURE_H
#define ARRAY_BINDING_FIXTURE_H
static const char array_binding_source[]=
    "fn assert(value:bool){print(\"shadow must not run\")}\n"
    "var calls=0\nfn make()->Array<i64>{calls+=1;return [10,20,30]}\n"
    "var [topA,_,topC]=make()\nconst [topString]=[\"A\\0B\"]\n"
    "const []=Array<i64>()\nconst [_]=[99]\n"
    "export fn answer()->i64{var [a,b,c]=[1,2,3];a=4;const [one]=[7];"
    "var []=Array<i64>();var [,,last]=[8,9,10];"
    "var person={first:\"Bob\",score:99};var {first,score}=person;"
    "var nested=[[5]];var [child]=nested;child[0]=6;"
    "return a==4&&b==2&&c==3&&one==7&&last==10&&calls==1&&"
    "topA==10&&topC==30&&first==\"Bob\"&&score==99&&nested[0][0]==5&&child[0]==6?42:0}\n"
    "export fn short()->i64{var [a,b]=[1];print(\"after mismatch\");return a+b}\n"
    "export fn long()->i64{const [a]=[1,2];print(\"after mismatch\");return a}\n"
    "export fn skipShort()->i64{var [_,_]=[1];print(\"after mismatch\");return 0}\n"
    "export fn emptyLong()->i64{var []=[1];print(\"after mismatch\");return 0}\n"
    "export fn kept()->string{return topString}\n";
static const char *const array_binding_names[]={"answer","short","long","skipShort","emptyLong","kept"};
static const char array_variables_expected[]=
    "count = 1\nPI = 3.14159\nAlice, age 25, height 1.75, active: true\n"
    "maybe = null\nmaybe = 42\nvalue = 42\n17 / 5 = 3 remainder 2\n"
    "a=10, b=20, c=30\nBob, score 99\n";
#endif
