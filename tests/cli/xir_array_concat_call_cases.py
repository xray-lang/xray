"""Require authentic variadic Array READ arguments and retained snapshots."""

CASES = [
 ('context',
  'fn combine<T>(seed:Array<T>,left:Array<T>,right:Array<T>)->Array<T>{'
  'return seed.concat(...(),...(left,right),...())}\n'
  'export fn inspect()->bool{const seed:Array<i8>=[1];'
  'const result=combine<i8>(seed,[2],[3]);const empty:Array<i8>=[];'
  'const contextual=empty.concat(...([],[4]),...());'
  'const nested=seed.concat(...(...([5],),[6]));'
  'return len(seed)==1 && seed[0]==1 && len(result)==3 && result[0]==1 && '
  'result[1]==2 && result[2]==3 && len(contextual)==1 && contextual[0]==4 && '
  'len(nested)==3 && nested[1]==5 && nested[2]==6}\n',
  'import {inspect} from "./context-values"\nprint(comptime inspect())\n',b'true\n'),
 ('tuple-result',
  'fn pair<T>(left:Array<T>,right:Array<T>)->(Array<T>,Array<T>){return (left,right)}\n'
  'export fn inspectPair()->bool{const seed:Array<i8>=[1];'
  'const result=seed.concat(...pair<i8>([2],[3]),[4],...pair<i8>([],[5]));'
  'return len(result)==5 && result[0]==1 && result[1]==2 && result[2]==3 && '
  'result[3]==4 && result[4]==5 && len(seed)==1}\n',
  'import {inspectPair} from "./tuple-result-values"\nprint(comptime inspectPair())\n',b'true\n'),
 ('growth',
  'export fn inspectGrowth()->bool{const seed:Array<i8>=[1];'
  'const result=seed.concat(...([2],[3],[4],[5],[6],[7],[8],[9],[10]),'
  '...(),...([11],[12],[13],[14],[15],[16],[17],[18],[19]));'
  'return len(result)==19 && result[0]==1 && result[7]==8 && result[8]==9 && '
  'result[15]==16 && result[16]==17 && result[18]==19 && len(seed)==1 && seed[0]==1}\n',
  'import {inspectGrowth} from "./growth-values"\nprint(comptime inspectGrowth())\n',b'true\n'),
 ('zero',
  'fn empty()->(){return ()}\n'
  'export fn inspectZero()->bool{const seed:Array<i8>=[1,2];'
  'const result=seed.concat(...empty(),...(),...empty());'
  'const plain=seed.concat();return len(result)==2 && result[0]==1 && '
  'result[1]==2 && len(plain)==2 && plain[0]==1 && plain[1]==2}\n',
  'import {inspectZero} from "./zero-values"\nprint(comptime inspectZero())\n',b'true\n'),
 ('order-runtime',
  'export fn marker()->i64{return 1}\n',
  'import {marker} from "./order-runtime-values"\n'
  'var trace:i64=0\nvar target:Array<i64>=[1,2]\nconst old=target\n'
  'fn empty(tag:i64)->(){trace=trace*10+tag;if(tag==marker()){target=[3]};return ()}\n'
  'fn parts()->(Array<i64>,Array<i64>){trace=trace*10+2;target[0]=4;return (target,[5])}\n'
  'fn tail()->Array<i64>{trace=trace*10+4;target=[6];return target}\n'
  'const result=target.concat(...empty(1),target,...parts(),...empty(3),tail(),...empty(5))\n'
  'print(trace,len(result),result[0],result[1],result[2],result[3],result[4],result[5],'
  'len(old),old[0],target[0])\n',b'12345 6 1 2 3 4 5 6 2 1 6\n')]

NEGATIVE = [
 ('scalar','const xs:Array<i64>=[1]\nprint(xs.concat(...7))\n',b'call spread requires a statically known Tuple'),
 ('array','const xs:Array<i64>=[1]\nprint(xs.concat(...xs))\n',b'call spread requires a statically known Tuple'),
 ('ref','const xs:Array<i64>=[1]\nvar ys:Array<i64>=[2]\nprint(xs.concat(ref ys))\n',b'direct call arguments must use the declared READ contract'),
 ('type','const xs:Array<i8>=[1]\nprint(xs.concat(...(["bad"],)))\n',b'expression cannot satisfy its declared type'),
 ('unit','const xs:Array<i8>=[1]\nprint(xs.concat(()))\n',b'empty Tuple syntax denotes Unit'),
 ('type-arguments','const xs:Array<i8>=[1]\nprint(xs.concat<i8>([2]))\n',b'Array concat takes ordinary trailing Array arguments'),
 ('io','fn empty()->(){print(7);return ()}\nfn answer()->i64{const xs:Array<i64>=[1];'
  'return xs.concat(...empty())[0]}\nprint(comptime answer())\n',b'admitted deterministic scalar direct-call closure')]

SEMANTICS = {name:('ordinary_runtime_zero_width_evaluation' if name=='order-runtime' else 'compiler_constant')
             for name,_,_,_ in CASES}
