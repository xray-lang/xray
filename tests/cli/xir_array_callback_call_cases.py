"""Require all Array callbacks through authentic ordered Tuple arguments."""

CASES = [
 ('methods',
  'export fn inspectMethods()->bool{const xs:Array<i8>=[1,2,3];'
  'const mapped=xs.map(...(),...(fn(x){return x+1},),...());'
  'const indexed=xs.map(...(...(fn(x,index){return index},),));'
  'const chosen=xs.filter(...(fn(x,index){return x>1 && index>0},));'
  'var seen:i64=0;xs.forEach(...(fn(x){seen=seen+(x as i64)},));'
  'const found=xs.find(...(fn(x){return x==2},));'
  'const position=xs.findIndex(...(fn(x){return x==3},));'
  'const all=xs.every(...(fn(x){return x>0},));'
  'const any=xs.some(...(fn(x){return x==3},));'
  'const total=xs.reduce(...(fn(a,x){return a+(x as i64)},0));'
  'return mapped[0]==2 && mapped[2]==4 && indexed[0]==0 && indexed[2]==2 && '
  'len(chosen)==2 && chosen[0]==2 && chosen[1]==3 && seen==6 && '
  '(found!)==2 && position==2 && all && any && total==6}\n',
  'import {inspectMethods} from "./methods-values"\nprint(comptime inspectMethods())\n',b'true\n'),
 ('context',
  'fn copied<T>(xs:Array<T>)->Array<T>{return xs.map(...(fn(x){return x},))}\n'
  'export fn inspectContext()->bool{const xs:Array<i8>=[1,2,3];'
  'const copy=copied<i8>(xs);const words=xs.map(...(fn(x){return x.toString()},));'
  'const initial:i8=4;const small=xs.reduce(...(fn(a,x){return a+x},...(initial,)));'
  'const total=xs.reduce(...(fn(a,x){return a+(x as i64)},...(0,)));'
  'return copy[0]==1 && copy[2]==3 && words[0]=="1" && words[2]=="3" && '
  'small==10 && total==6}\n',
  'import {inspectContext} from "./context-values"\nprint(comptime inspectContext())\n',b'true\n'),
 ('typed-values',
  'fn mapper()->(fn(i64)->i64,){return (fn(x:i64)->i64{return x*2},)}\n'
  'fn reducer()->(fn(i64,i64)->i64,i64){return (fn(a:i64,b:i64)->i64{return a+b},4)}\n'
  'export fn inspectTyped()->bool{const xs:Array<i64>=[1,2,3];'
  'const mapped=xs.map(...mapper());const total=xs.reduce(...reducer());'
  'return mapped[0]==2 && mapped[2]==6 && total==10}\n',
  'import {inspectTyped} from "./typed-values-values"\nprint(comptime inspectTyped())\n',b'true\n'),
 ('empty',
  'export fn inspectEmpty()->bool{const xs:Array<i64>=[];var seen:i64=0;'
  'xs.forEach(...(),...(fn(x,index){seen=seen+x+index},),...());'
  'const none=xs.find(...(fn(x){return x==1},));'
  'return xs.every(...(fn(x){return false},)) && !xs.some(...(fn(x){return true},)) && '
  'xs.findIndex(...(fn(x){return true},))==(-1) && none==null && seen==0 && '
  'xs.reduce(...(fn(a,x){return a+x},7))==7}\n',
  'import {inspectEmpty} from "./empty-values"\nprint(comptime inspectEmpty())\n',b'true\n'),
 ('nullable',
  'export fn inspectNullable()->bool{const xs:Array<i64>=[1,2];var initial:i64?=4;'
  'if(initial!=null){const plain=xs.reduce(fn(a,x){return a+x},initial);'
  'const spreadTotal=xs.reduce(...(fn(a,x){return a+x},initial));return plain==7 && spreadTotal==7};'
  'return false}\n',
  'import {inspectNullable} from "./nullable-values"\nprint(comptime inspectNullable())\n',b'true\n'),
 ('packed-runtime',
  'export fn markerPacked()->i64{return 1}\n',
  'import {markerPacked} from "./packed-runtime-values"\n'
  'var tracePacked:i64=0\nvar targetPacked:Array<i64>=[1,2]\nconst oldPacked=targetPacked\n'
  'fn receiverPacked()->Array<i64>{tracePacked=tracePacked*10+markerPacked();return targetPacked}\n'
  'fn emptyPacked(tag:i64)->(){tracePacked=tracePacked*10+tag;if(tag==2){targetPacked=[5]};return ()}\n'
  'fn callbackPacked()->fn(i64,i64)->i64{tracePacked=tracePacked*10+3;const delta=targetPacked[0];'
  'return fn(a:i64,x:i64)->i64{return a+x+delta}}\n'
  'fn initialPacked()->i64{tracePacked=tracePacked*10+5;targetPacked=[9];return 4}\n'
  'const totalPacked=receiverPacked().reduce(...(...emptyPacked(2),callbackPacked(),'
  '...emptyPacked(4),initialPacked(),...emptyPacked(6)))\n'
  'print(tracePacked,totalPacked,len(oldPacked),oldPacked[0],targetPacked[0])\n',b'123456 17 2 1 9\n'),
 ('order-runtime',
  'export fn marker()->i64{return 1}\n',
  'import {marker} from "./order-runtime-values"\n'
  'var trace:i64=0\nvar target:Array<i64>=[1,2]\nconst old=target\n'
  'fn receiver()->Array<i64>{trace=trace*10+marker();return target}\n'
  'fn empty(tag:i64)->(){trace=trace*10+tag;if(tag==2){target=[5]};return ()}\n'
  'fn callback()->(fn(i64,i64)->i64,){trace=trace*10+3;const delta=target[0];'
  'return (fn(a:i64,x:i64)->i64{return a+x+delta},)}\n'
  'fn initial()->i64{trace=trace*10+5;target=[9];return 4}\n'
  'const total=receiver().reduce(...empty(2),...callback(),...empty(4),initial(),...empty(6))\n'
  'print(trace,total,len(old),old[0],target[0])\n',b'123456 17 2 1 9\n')]

NEGATIVE = [
 ('scalar','const xs:Array<i64>=[1]\nprint(xs.map(...1))\n',b'call spread requires a statically known Tuple'),
 ('array','const xs:Array<i64>=[1]\nprint(xs.map(...xs))\n',b'call spread requires a statically known Tuple'),
 ('arity','const xs:Array<i64>=[1]\nprint(xs.map(...(fn(x){return x},7)))\n',b'too many call arguments'),
 ('ref','const xs:Array<i64>=[1]\nvar f:fn(i64)->i64=fn(x){return x}\nprint(xs.map(ref f))\n',b'direct call arguments must use the declared READ contract'),
 ('parameter','const xs:Array<i64>=[1]\nprint(xs.map(...(fn(x:string)->string{return x},)))\n',b'callable conversion may only weaken its outer execution contract'),
 ('map-unit','const xs:Array<i64>=[1]\nprint(xs.map(...(fn(x){return ()},)))\n',b'map callback must produce a value'),
 ('predicate','const xs:Array<i64>=[1]\nprint(xs.some(...(fn(x)->i64{return x},)))\n',b'callable conversion may only weaken its outer execution contract'),
 ('unit','const xs:Array<i64>=[1]\nxs.forEach(...(),())\n',b'unit argument is not admitted'),
 ('io','fn args()->(fn(i64)->i64,){print(7);return (fn(x:i64)->i64{return x},)}\n'
  'fn answer()->i64{const xs:Array<i64>=[1];return xs.map(...args())[0]}\nprint(comptime answer())\n',
  b'admitted deterministic scalar direct-call closure')]

SEMANTICS = {name:('ordinary_runtime_zero_width_evaluation' if name.endswith('runtime') else 'compiler_constant')
             for name,_,_,_ in CASES}
