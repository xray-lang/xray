"""Require Array callbacks across compiler errors, panic cleanup and private Classes."""

CASES = [
 ('error',
  'enum Fault{Stop}\n'
  'export fn errorCalls()->i64{var seen:i64=0;const xs:Array<i64>=[1,2,3];'
  'try{const ys=xs.map(...(fn(x){defer{seen=seen*10+x};'
  'if(x==2){throw Fault.Stop};return x},));return -1}'
  'catch(e:Fault){return seen*10+7}}\n',
  'import {errorCalls} from "./error-values"\nprint(comptime errorCalls())\n',b'127\n'),
 ('panic',
  'export fn panicCalls()->bool{var seen:i64=0;const xs:Array<i64>=[1,0,2];'
  'try{xs.forEach(...(fn(x){defer{seen=seen*10+1};const y=8/x;seen=seen+y},));return false}'
  'catch panic(p){return seen==811 && p.code==420}}\n',
  'import {panicCalls} from "./panic-values"\nprint(comptime panicCalls())\n',b'true\n'),
 ('class',
  'class Counter{value:i64;init(value:i64){this.value=value};'
  'add(x:i64)->i64{this.value=this.value+x;return this.value}}\n'
  'export fn classCalls()->bool{const counter=Counter(1);const alias=counter;'
  'const xs:Array<i64>=[2,3];const out=xs.map(...(fn(x){return alias.add(x)},));'
  'return out[0]==3 && out[1]==6 && counter.value==6}\n',
  'import {classCalls} from "./class-values"\nprint(comptime classCalls())\n',b'true\n'),
 ('class-array',
  'class Buffer{items:Array<i8>;init(){this.items=[1,2]}}\n'
  'fn grow(xs:ref Array<i8>){xs.push(...(3,))}\n'
  'export fn classArrayCalls()->bool{const buffer=Buffer();const alias=buffer;'
  'const old=buffer.items;grow(ref buffer.items);buffer.items.reverse(...());'
  'const total=buffer.items.reduce(...(fn(a,x){return a+(x as i64)},0));'
  'return len(old)==2 && old[0]==1 && len(alias.items)==3 && alias.items[0]==3 && total==6}\n',
  'import {classArrayCalls} from "./class-array-values"\nprint(comptime classArrayCalls())\n',b'true\n'),
 ('bounds',
  'export fn boundsCalls()->bool{var seen:i64=0;const xs:Array<i64>=[1,2,3];'
  'try{const ys=xs.map(...(fn(x){seen=seen+x;if(x==2){return xs[99]};return x},));return false}'
  'catch panic(p){return seen==3 && len(xs)==3 && p.code==430}}\n',
  'import {boundsCalls} from "./bounds-values"\nprint(comptime boundsCalls())\n',b'true\n')]

NEGATIVE = [
 ('class-export','class Box{value:i8;init(value:i8){this.value=value}}\n'
  'fn make()->Box{return Box(7)}\nconst value=comptime make()\nprint(value.value)\n',
  b'comptime result must belong to an admitted closed value family'),
 ('io','fn answer()->i64{const xs:Array<i64>=[1];return xs.map(...(fn(x){print(x);return x},))[0]}\n'
  'print(comptime answer())\n',b'admitted deterministic scalar direct-call closure')]
