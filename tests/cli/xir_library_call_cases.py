"""Independent standard-library call and compiler-owned callback requirements."""

CASES = [
 ('native',
  'fn has<T:Equal>(xs:Array<T>,needle:T)->bool{return xs.contains(...(needle,))}\n'
  'export fn answer()->bool{const xs:Array<i64>=[3,7];const n:i8=127;const f:f64=-2.5;const r:rune=\'A\';'
  'return has<i64>(xs,7) && xs.indexOf(...(3,))==0 && n.wrappingAdd(...(1,))==(-128 as i8) && '
  '7.wrappingAdd(...(3,)).max(...(9,))==10 && "abc".contains(...("b",)) && '
  '"abcabc".indexOf(...("bc",2))==4 && "a\\0".repeat(...(2,))=="a\\0a\\0" && '
  'f.abs(...())==2.5 && r.toUInt32(...())==65 && (true).toString(...())=="true"}\n'
  'export fn square()->f64{return 9.sqrt(...())}\n',
  'import {answer,square} from "./native-values"\nprint(comptime answer(),comptime square())\n',
  b'true 3.0\n'),
 ('arrays',
  'fn append<T>(xs:ref Array<T>,value:T){xs.push(...(value,))}\n'
  'export fn inspect()->bool{var xs=Array<i64>.withCapacity(...(4,));append<i64>(ref xs,3);'
  'xs.set(...(0,7));const old=xs;xs.reserve(...(8,));xs.set(...(0,9));'
  'return old.get(...(0,))==7 && xs.get(...(0,))==9 && xs.capacity>=8}\n',
  'import {inspect} from "./arrays-values"\nprint(comptime inspect())\n',
  b'true\n'),
 ('functions',
  'export fn table()->Array<i8>{const delta:i8=2;const xs:Array<i8>=[1,2,3];'
  'return xs.map(fn(x:i8)->i8{return x+delta})}\n'
  'export fn total()->i64{const xs:Array<i64>=[1,2,3];const mapped=xs.map(fn(x:i64)->i64{return x*2});'
  'const chosen=mapped.filter(fn(x:i64)->bool{return x>2});return chosen.reduce(fn(a:i64,b:i64)->i64{return a+b},0)}\n',
  'import {table,total} from "./functions-values"\nconst p=comptime table()\nprint(p[0],p[1],p[2],comptime total())\n',
  b'3 4 5 10\n'),
 ('cleanup',
  'fn snapshot()->string{var x="a";defer{x=x+"b"};return x}\n'
  'export fn text()->string{return snapshot()}\n'
  'export fn order()->i64{var trace:i64=0;{defer{trace=trace*10+1};defer{trace=trace*10+2}};return trace}\n',
  'import {text,order} from "./cleanup-values"\nprint(comptime text(),comptime order())\n',
  b'a 21\n'),
 ('order-runtime',
  'export fn marker()->i64{return 1}\n',
  'import {marker} from "./order-runtime-values"\nvar trace:i64=0\n'
  'fn receiver()->string{trace=trace*10+marker();return "aba"}\n'
  'fn empty()->(){trace=trace*10+2;return ()}\n'
  'fn pattern()->(string,){trace=trace*10+3;return ("b",)}\n'
  'const found=receiver().contains(...empty(),...pattern(),...empty())\n'
  'var xs:Array<i64>=[3,7]\nconst old=xs\nxs.clear(...empty())\n'
  'var target:Array<i64>=[1]\nfn index()->(i64,){target=[9];return (0,)}\n'
  'const before=target.get(...index())\nprint(trace,found,len(xs),len(old),before,target.get(0))\n',
  b'12322 true 0 2 1 9\n')]

NEGATIVE = [
 ('scalar', 'print(7.wrappingAdd(...1))\n', b'call spread requires a statically known Tuple'),
 ('arity', 'print(7.wrappingAdd(...(1,2)))\n', b'too many call arguments'),
 ('ref', 'var x:i64=1\nprint(7.wrappingAdd(ref x))\n', b'direct call arguments must use the declared READ contract'),
 ('unit', 'print(7.abs(()))\n', b'integer operation requires its exact ordinary declaration'),
 ('io', 'fn arguments()->(i64,){print(7);return (1,)}\nfn answer()->i64{return 7.wrappingAdd(...arguments())}\n'
  'print(comptime answer())\n', b'admitted deterministic scalar direct-call closure')]

SEMANTICS = {name:('ordinary_runtime_zero_width_evaluation' if name=='order-runtime' else 'compiler_constant')
             for name,_,_,_ in CASES}
