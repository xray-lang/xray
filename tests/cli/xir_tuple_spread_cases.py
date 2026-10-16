"""Independent compiler and runtime Tuple spread requirements."""

CASES = [
 ('owned',
  'fn seed()->(i8,string){return (7,"a\\0")}\n'
  'export fn make()->((),i8,string,i8){const p=seed();return ((),...p,3)}\n',
  'import {make} from "./owned-values"\nconst p=comptime make()\nprint(p.1,p.2,p.3)\n',
  b'7 a\x00 3\n'),
 ('context',
  'fn widen<T>(pair:(T,i8))->((),T,i64,()){return ((),...pair,())}\n'
  'export fn inspect()->bool{const p=widen<string>(("é\\0",7));'
  'const one=(...(...(11 as i8,),),);return p.1=="é\\0" && p.2==7 && one.0==11}\n',
  'import {inspect} from "./context-values"\nprint(comptime inspect())\n',
  b'true\n'),
 ('empty-runtime',
  'export fn seven()->i64{return 7}\n',
  'import {seven} from "./empty-runtime-values"\nvar trace:i64=0\n'
  'fn empty()->(){trace=trace*10+1;return ()}\n'
  'fn marker()->(){trace=trace*10+2;return ()}\n'
  'const nothing:()=(...empty(),)\nconst p=(marker(),...empty(),...(),seven())\n'
  'print(trace,p.1)\n',
  b'121 7\n'),
 ('unit-parameter',
  'fn pass(empty:(),value:i8)->i8{return value}\n'
  'export fn answer()->i8{return pass((),7)}\n',
  'import {answer} from "./unit-parameter-values"\nprint(comptime answer())\n',
  b'7\n')]

NEGATIVE = [
 ('scalar', 'const p=(...7,)\nprint(0)\n', b'Tuple literal spread requires a statically known Tuple'),
 ('arity', 'const p:(i8,i8)=(...(1 as i8,),)\nprint(0)\n', b'Tuple literal requires an exact ordered arity context'),
 ('type', 'const p:(i8,string)=(...(1 as i8,2 as i8),)\nprint(0)\n', b'expression cannot satisfy its declared type'),
 ('private-import', 'import {seed} from "./owned-values"\nprint(0)\n', b'import requires an exported declaration')]

SEMANTICS = {name:('ordinary_runtime_zero_width_evaluation' if name=='empty-runtime' else 'compiler_constant')
             for name,_,_,_ in CASES}
