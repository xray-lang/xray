/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * tuple_assignment_fixture.h - Independent Tuple output and finite ownership scenarios
 */
#ifndef TUPLE_ASSIGNMENT_FIXTURE_H
#define TUPLE_ASSIGNMENT_FIXTURE_H
static const char tuple_assignment_source[]=
    "var first=1;var second=2\n"
    "(first,second)=(second,first)\n"
    "var calls=0\n"
    "fn once()->(i64,i64){calls+=1;return (7,8)}\n"
    "var onceA=0;var onceB=0\n"
    "(onceA,onceB)=once()\n"
    "const absent:i64?=null\n"
    "var collections=0\n"
    "fn pairs()->Array<(string,i64)>{collections+=1;return [(\"a\",1),(\"b\",2),(\"c\",3)]}\n"
    "fn facts()->i64{var n:i64?=1;var times=0;while(n!=null){(n,times)=(absent,times+1)};return times}\n"
    "fn captured()->i64{var n=1;var change=fn(){(n,)=(2,)};change();return n}\n"
    "fn loopCaptured()->i64{var fs:Array<fn()->i64>=[];for((_,v) in [(1,10),(2,20)]){fs.push(fn()->i64{return v})};return fs[0]()+fs[1]()}\n"
    "fn early()->i64{for((_,v) in [(1,3)]){return v};return 0}\n"
    "export fn answer()->i64{\n"
    "var a=3;var b=4;(a,b)=(b,a);(a,_)=(5,\"ignored\");var one=0;(one,)=(9,)\n"
    "var xs=[1];var ys=[2];const saved=xs;(xs,ys)=(ys,xs);xs[0]=9\n"
    "var before=collections;var seq=pairs();var total=0\n"
    "for((key,value) in seq){total+=value;seq=[(\"z\",99)]}\n"
    "var steps=0\n"
    "for((_,v) in [(1,1),(2,2),(3,3)]){if(v==2){continue};if(v==3){break};steps+=v}\n"
    "for((_,_) in Array<(i64,i64)>()){steps+=100}\n"
    "return first==2&&second==1&&calls==1&&onceA==7&&onceB==8&&a==5&&b==3&&one==9&&xs[0]==9&&ys[0]==1&&saved[0]==1&&total==6&&collections==before+1&&steps==1&&facts()==1&&captured()==2&&loopCaptured()==30&&early()==3?42:0\n"
    "}\n"
    "export fn kept()->string{var left=\"A\\0B\";var right=\"ok\";(left,right)=(right,left);return right}\n";
static const char *const tuple_assignment_names[]={"answer","kept"};
static const char tuple_demo_expected[]="(1, \"hello\")\ntrue\n3.14\ngo\n3, 4\nscore=100\n10\n20\nlabel\n17/5 = 3 rem 2\nfirst slot only: 3\nafter swap: a=2, b=1\na=1\nb=2\nc=3\norigin\ny-axis\nx-axis\ndiagonal\ngeneral\n42\n-1\ndist^2 = 25\nconcat.0 = 1, concat.4 = 5\n7\ntrue\n";
#endif
