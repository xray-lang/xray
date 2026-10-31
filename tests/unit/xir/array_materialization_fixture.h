/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * array_materialization_fixture.h - Independent complete output and owned range controls
 */
#ifndef ARRAY_MATERIALIZATION_FIXTURE_H
#define ARRAY_MATERIALIZATION_FIXTURE_H
static const char array_materialization_source[]=
    "var optional:i64?=7\n"
    "var pair:(string,i64)=(\"ok\",2)\n"
    "var lookup:Map<string,i64>=#{\"x\":3}\n"
    "var members:Set<i64>=#[4]\n"
    "var ticks=0\n"
    "fn lower()->i64{ticks=ticks*10+1;return 1}\n"
    "fn upper()->i64{ticks=ticks*10+2;return 3}\n"
    "var byteOwner:Array<u8>=[10,20,30]\n"
    "fn replaceBytes()->i64{byteOwner=[40,50];return 0}\n"
    "fn borrowOrder()->i64{byteOwner=[10,20,30];var v:Slice<u8>=byteOwner[replaceBytes():];return len(v)*100+((v[0]) as i64)}\n"
    "fn copyOrder()->i64{byteOwner=[10,20,30];var v=copy byteOwner[replaceBytes():];return len(v)*100+((v[0]) as i64)+(byteOwner[0]==40?0:999)}\n"
    "enum RangeError{Bad}\n"
    "fn failLower() throws(RangeError)->i64{ticks=ticks*10+3;throw RangeError.Bad}\n"
    "fn laterUpper()->i64{ticks=ticks*10+4;return 2}\n"
    "fn borrowError()->i64{ticks=0;try{var v:Slice<u8>=byteOwner[try failLower():laterUpper()];return len(v)}catch(e:RangeError){return ticks==3?1:0}}\n"
    "fn copyError()->i64{ticks=0;try{var v=copy byteOwner[try failLower():laterUpper()];return len(v)}catch(e:RangeError){return ticks==3?1:0}}\n"
    "export fn answer()->i64{\n"
    " var xs=[3,1,2];const old=xs;var sorted=xs.sort()\n"
    " var part=copy xs[0:2];xs[0]=8;part[0]=9\n"
    " var all=copy xs[:];var tail=copy xs[1:];var head=copy xs[:2];var empty=copy xs[1:1]\n"
    " ticks=0;var ordered=copy xs[lower():upper()]\n"
    " var bytes:Array<u8>=[7,9];var bytecopy=copy bytes[:];bytes[0]=1;bytecopy[1]=8\n"
    " var single=[1];single.sort();var none:Array<i64>=[];none.sort()\n"
    " var mixed=[3,-1,3,0];const unchanged=mixed;var alias=mixed.sort();mixed[0]=11\n"
    " var lower=copy xs[-9223372036854775808:9223372036854775807]\n"
    " var higher=copy xs[9223372036854775807:];var below=copy xs[:-9223372036854775808]\n"
    " var plain=(optional,pair,lookup,members)\n"
    " return sorted[0]==1&&sorted[1]==2&&sorted[2]==3&&old[0]==3&&part[0]==9&&xs[0]==8&&all[0]==8&&len(tail)==2&&len(head)==2&&len(empty)==0&&ticks==12&&ordered[0]==2&&ordered[1]==3&&bytecopy[0]==7&&bytecopy[1]==8&&bytes[1]==9&&single[0]==1&&len(none)==0&&alias[0]== -1&&alias[1]==0&&alias[2]==3&&alias[3]==3&&unchanged[0]==3&&plain.0==7&&len(lower)==3&&lower[0]==8&&len(higher)==0&&len(below)==0&&borrowOrder()==240&&copyOrder()==310&&borrowError()==1&&copyError()==1?42:0\n"
    "}\n"
    "export fn kept()->string{return \"A\\0B\"}\n"
    "export fn negative()->i64{var xs=[1];var out=copy xs[-1:];return len(out)}\n"
    "export fn reversed()->i64{var xs=[1,2];var out=copy xs[2:1];return len(out)}\n"
    "export fn oversized()->i64{var xs=[1];var out=copy xs[:2];return len(out)}\n"
    "export fn subview()->i64{var xs:Array<u8>=[1];var v:Slice<u8>=xs[:];var child:Slice<u8>=v[-1:];return len(child)}\n";
#define ARRAY_MATERIALIZATION_ENTRY_COUNT 6
static const char *const array_materialization_names[]={"answer","kept","negative","reversed","oversized","subview"};
static const char array_demo_expected[]="nums: [1, 2, 3, 4, 5]\nempty: []\nfirst: 1, last: 5\nafter push(6) + pop(): [1, 2, 3, 4, 5], popped: 6\nafter unshift(0) + shift(): [1, 2, 3, 4, 5], shifted: 0\nnums[1:4] = [2, 3, 4]\nnums[:3]  = [1, 2, 3]\nnums[3:]  = [4, 5]\nindexOf(3) = 2\nincludes(5) = true\nhello, world\nsorted: [1, 2, 3, 5, 8, 9]\nreversed: [9, 8, 5, 3, 2, 1]\nevens: [2, 4, 6, 8, 10]\ndoubled: [2, 4, 6, 8, 10, 12, 14, 16, 18, 20]\nsum: 55\neven squares: [4, 16, 36, 64, 100]\nfirst > 5: 6\nall positive: true\nhas > 8: true\nfilled: [7, 7, 7, 7, 7]\n";
#endif
