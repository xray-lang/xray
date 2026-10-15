"""Require ordinary arguments for every admitted Array mutation recipe."""

CASES = [
 ('mutation',
  'fn prepend<T>(xs:ref Array<T>,value:T){xs.unshift(...(value,))}\n'
  'fn grow<T>(xs:ref Array<T>,n:i64,value:T)->Array<T>{return xs.resize(...(n,value))}\n'
  'export fn inspect()->bool{var xs:Array<i8>=[1,2,3];const old=xs;'
  'const last=xs.pop(...());const first=xs.shift(...());prepend<i8>(ref xs,4);'
  'const backwards=xs.reverse(...());const larger=grow<i8>(ref xs,4,5);'
  'const filled=xs.fill(...(9,1,3));'
  'var empty:Array<i8>=[];const noLast=empty.pop(...());const noFirst=empty.shift(...());'
  'return (last!)==3 && (first!)==1 && len(old)==3 && old[0]==1 && old[2]==3 && '
  'len(backwards)==2 && backwards[0]==2 && backwards[1]==4 && '
  'len(larger)==4 && larger[0]==2 && larger[1]==4 && larger[3]==5 && '
  'filled[0]==2 && filled[1]==9 && filled[2]==9 && filled[3]==5 && '
  'noLast==null && noFirst==null}\n',
  'import {inspect} from "./mutation-values"\nprint(comptime inspect())\n',
  b'true\n'),
 ('defaults',
  'export fn inspectDefaults()->bool{var xs:Array<i8>=[1,2,3];'
  'xs.unshift(...(4,));const first=xs.fill(...(),...(7,),...());'
  'const second=xs.fill(...(8,2),...());const entries=xs.entries(...());'
  'const e0=entries[0];const e3=entries[3];'
  'return len(first)==4 && first[0]==7 && first[3]==7 && '
  'second[0]==7 && second[1]==7 && second[2]==8 && second[3]==8 && '
  'len(entries)==4 && e0.0==0 && e0.1==7 && e3.0==3 && e3.1==8}\n',
  'import {inspectDefaults} from "./defaults-values"\nprint(comptime inspectDefaults())\n',
  b'true\n'),
 ('projected',
  'fn mutate<T>(xs:ref Array<T>,value:T)->Array<T>{xs.unshift(...(value,));'
  'xs.reverse(...());xs.resize(...(4,value));return xs.fill(...(value,1,3))}\n'
  'export fn inspectNested()->bool{var rows:Array<Array<i8>>=[[1,2],[3]];const old=rows;'
  'const result=mutate<i8>(ref rows[0],7);const entries=rows[0].entries(...());'
  'return len(result)==4 && result[0]==2 && result[1]==7 && result[3]==7 && '
  'len(rows[0])==4 && rows[0][2]==7 && len(rows[1])==1 && rows[1][0]==3 && '
  'len(old[0])==2 && old[0][0]==1 && old[0][1]==2 && len(entries)==4}\n',
  'import {inspectNested} from "./projected-values"\nprint(comptime inspectNested())\n',
  b'true\n'),
 ('order-runtime',
  'export fn marker()->i64{return 1}\n',
  'import {marker} from "./order-runtime-values"\n'
  'var trace:i64=0\nvar target:Array<i64>=[1,2]\nconst old=target\n'
  'fn reset()->(){trace=trace*10+marker();target=[8,9];return ()}\n'
  'fn item()->(i64,){trace=trace*10+2;target=[3];return (7,)}\n'
  'fn grow()->(i64,i64){trace=trace*10+3;target=[4,5];return (3,6)}\n'
  'fn fill()->(i64,i64){trace=trace*10+4;target=[1,2,3,4];return (8,1)}\n'
  'fn empty(tag:i64)->(){trace=trace*10+tag;return ()}\n'
  'fn readReset()->(){trace=trace*10+7;target=[6];return ()}\n'
  'const removed=target.pop(...reset())\ntarget.unshift(...item())\n'
  'const resized=target.resize(...grow())\nconst filled=target.fill(...fill())\n'
  'const shifted=target.shift(...empty(5))\nconst reversed=target.reverse(...empty(6))\n'
  'const snapshot=target.entries(...readReset())\n'
  'print(trace,removed!,len(old),old[0],len(resized),resized[2],len(filled),filled[3],'
  'shifted!,len(reversed),len(snapshot),target[0])\n',
  b'1234567 9 2 1 3 6 4 8 1 3 3 6\n')]

NEGATIVE = [
 ('scalar','var xs:Array<i8>=[1]\nxs.unshift(...7)\n',b'call spread requires a statically known Tuple'),
 ('arity','var xs:Array<i8>=[1]\nxs.fill(...(1,0,1,2))\n',b'too many call arguments'),
 ('type','var xs:Array<i8>=[1]\nxs.resize(...(2,"bad"))\n',b'expression cannot satisfy its declared type'),
 ('readonly','const xs:Array<i64>=[1]\nxs.reverse(...())\n',b'value mutation requires a mutable named root'),
 ('ref','var xs:Array<i64>=[1]\nvar n:i64=2\nxs.unshift(ref n)\n',b'direct call arguments must use the declared READ contract'),
 ('unit','var xs:Array<i64>=[1]\nxs.pop(())\n',b'Array removal requires no explicit arguments'),
 ('entries-arity','const xs:Array<i64>=[1]\nprint(xs.entries(...(1,)))\n',b'too many call arguments'),
 ('io','fn empty()->(){print(7);return ()}\nfn answer()->i64{var xs:Array<i64>=[1,2];'
  'xs.reverse(...empty());return xs[0]}\nprint(comptime answer())\n',b'admitted deterministic scalar direct-call closure')]

SEMANTICS = {name:('ordinary_runtime_zero_width_evaluation' if name=='order-runtime' else 'compiler_constant')
             for name,_,_,_ in CASES}
