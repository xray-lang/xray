"""Independent owned display, evaluation and compiler-constant observations."""
CASES = [
    ('owned-text-runtime',
     'enum Shade{Red,Green}\nexport fn render()->string{const n:i64?=null;const p:i64?=7;'
     'return "n=${n} p=${p} enum=${Shade.Green} array=${[1,2]}"}\n',
     'import {render} from "./owned-text-runtime-values"\nprint(render())\n',
     b'n=null p=7 enum=Shade.Green array=[1, 2]\n'),
    ('owned-text-cast',
     'export fn render()->string{const values:Array<i8>=[-7,8];return string(values)}\n',
     'import {render} from "./owned-text-cast-values"\nprint(render())\n',b'[-7, 8]\n'),
    ('owned-text-comptime',
     'export fn render()->string{const n:i64?=7;const absent:i64?=null;'
     'return string(n)+"|"+string(absent)+"|"+string([1,2])}\n',
     'import {render} from "./owned-text-comptime-values"\nconst folded=comptime render()\nprint(folded)\n',
     b'7|null|[1, 2]\n'),
    ('owned-text-unit',
     'export fn render(empty:())->string{return string(empty)}\n',
     'import {render} from "./owned-text-unit-values"\nprint(render(()))\n'
     'const folded=comptime render(())\nprint(folded)\n',b'()\n()\n'),
    ('owned-text-payload',
     'enum Message{Empty,Data{text:string,nums:Array<i64>}}\n'
     'export fn render()->string{return string(Message.Data{text:"A\\0中",nums:[-7,8]})}\n',
     'import {render} from "./owned-text-payload-values"\nprint(render())\n',
     'Message.Data("A\0中", [-7, 8])\n'.encode()),
    ('owned-text-float-payload',
     'enum Number{Value{value:f64}}\nexport fn render()->string{return string(Number.Value{value:1.5})}\n',
     'import {render} from "./owned-text-float-payload-values"\nprint(render())\n'
     'const folded=comptime render()\nprint(folded)\n',b'Number.Value(1.5)\nNumber.Value(1.5)\n'),
]

NEGATIVE = [
    ('text-function','fn answer()->i64{return 42}\nprint(string(answer))\n',b'value has no admitted string conversion'),
    ('text-class','class Box{value:i64;init(value:i64){this.value=value}}\nprint(string(Box(1)))\n',
     b'value has no admitted string conversion'),
    ('text-struct','struct Pair{x:i64}\nprint(string(Pair{x:1}))\n',b'value has no admitted string conversion'),
    ('text-inactive-function-payload','enum Choice{Empty,Callback{f:fn()->i64}}\nprint(string(Choice.Empty))\n',
     b'value has no admitted string conversion'),
    ('text-null-context','print(string(null))\n',b'null requires an exact nullable context'),
    ('text-null-template-context','print("${null}")\n',b'null requires an exact nullable context'),
]
