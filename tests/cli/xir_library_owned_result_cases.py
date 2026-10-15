"""Consume complete owning library results under the authorized output policy."""

CASES = [
 ('mapped-array',
  'export fn mapped()->Array<i64>{const xs:Array<i64>=[1,2,3,4];return xs.map(fn(x){return x*3})}\n',
  'import {mapped} from "./mapped-array-values"\n'
  'const xs=comptime mapped();var ys=xs;ys.push(99);print(len(xs),xs[0],xs[3],len(ys),ys[4])\n',b'4 3 12 5 99\n'),
 ('repeated-text',
  'export fn repeated()->string{return "中\\0ab".repeat(3)}\n',
  'import {repeated} from "./repeated-text-values"\nprint(comptime repeated())\n',
  ('中\0ab'*3+'\n').encode()),
 ('wide-pair',
  'export fn pair()->(f64,i64){return ((81 as i64).sqrt(),(-9).abs())}\n',
  'import {pair} from "./wide-pair-values"\nconst p=comptime pair();print(p.0,p.1,p.0+(p.1 as f64))\n',b'9.0 9 18.0\n'),
 ('class-grown-array',
  'class Buffer{items:Array<(i64,i64)>;init(items:Array<(i64,i64)>){this.items=items}}\n'
  'fn append(xs:ref Array<(i64,i64)>){xs.push((1,1))}\n'
  'export fn changed()->Array<(i64,i64)>{const b=Buffer([(20,20)]);const old=b.items;'
  'append(ref b.items);if(len(old)!=1){return []};return b.items}\n',
  'import {changed} from "./class-grown-array-values"\nconst xs=comptime changed();'
  'print(len(xs),xs[0].0,xs[0].1,xs[1].0,xs[1].1)\n',b'2 20 20 1 1\n'),
 ('owning-words',
  'export fn words()->Array<string>{const xs:Array<string>=["abcdef","中\\0ab"];'
  'return xs.map(fn(x){return x.repeat(2)})}\n',
  'import {words} from "./owning-words-values"\nconst xs=comptime words();print(len(xs),xs[0],xs[1])\n',
  b'2 abcdefabcdef '+('中\0ab'*2+'\n').encode())]

NEGATIVE = [
 ('class-identity','class Box{value:i8;init(value:i8){this.value=value}}\n'
  'fn make()->Box{return Box(7)}\nconst value=comptime make()\nprint(value.value)\n',
  b'comptime result must belong to an admitted closed value family'),
 ('io','fn text()->string{print(1);return "complete library result"}\n'
  'print(comptime text())\n',b'admitted deterministic scalar direct-call closure')]
