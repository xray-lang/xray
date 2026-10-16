"""Independent logical arity, order and owning callable Unit consumers."""
CASES = [
    ('unit-order',
     'export fn mark(value:i64)->(){print(value)}\n'
     'export fn answer(first:(),left:i64,second:(),right:i64)->i64{return left+right}\n',
     'import {mark,answer} from "./unit-order-values"\nprint(answer(mark(1),19,mark(2),23))\n',
     b'1\n2\n42\n'),
    ('unit-function-value',
     'export fn answer(empty:())->i64{return 42}\n',
     'import {answer} from "./unit-function-value-values"\n'
     'const callback:fn(())->i64=answer\nprint(callback(()))\n',b'42\n'),
    ('unit-closure',
     'export fn make()->fn(())->i64{return fn(empty:())->i64{return 42}}\n',
     'import {make} from "./unit-closure-values"\nconst callback=make()\nprint(callback(()))\n',b'42\n'),
    ('unit-zero-capture',
     'fn answer(empty:())->i64{return 42}\n'
     'export fn make(empty:())->fn(())->i64{return fn(next:())->i64{return answer(empty)}}\n',
     'import {make} from "./unit-zero-capture-values"\nconst callback=make(())\nprint(callback(()))\n',b'42\n'),
    ('unit-struct-method',
     'export struct Box{value:i64;init(empty:()){this.value=42} answer(empty:())->i64{return this.value}}\n',
     'import {Box} from "./unit-struct-method-values"\nconst box=Box(())\n'
     'print(box.answer(()))\nconst callback:fn(())->i64=box.answer\nprint(callback(()))\n',b'42\n42\n'),
    ('unit-class-method',
     'export class Box{value:i64;init(empty:()){this.value=42} answer(empty:())->i64{return this.value}}\n',
     'import {Box} from "./unit-class-method-values"\nconst box=Box(())\n'
     'print(box.answer(()))\nconst callback:fn(())->i64=box.answer\nprint(callback(()))\n',b'42\n42\n'),
    ('unit-comptime',
     'fn answer(empty:())->i64{return 42}\nexport fn forward(empty:())->i64{return answer(empty)}\n',
     'import {forward} from "./unit-comptime-values"\nconst folded=comptime forward(())\nprint(folded)\n',b'42\n'),
    ('unit-higher-order',
     'export fn answer(empty:())->i64{return 42}\n'
     'export fn invoke(callback:fn(())->i64,empty:())->i64{return callback(empty)}\n',
     'import {answer,invoke} from "./unit-higher-order-values"\nprint(invoke(answer,()))\n',b'42\n'),
    ('unit-default',
     'fn mark()->(){print(1)}\nexport fn answer(empty:()=mark())->i64{return 42}\n',
     'import {answer} from "./unit-default-values"\nprint(answer())\nprint(answer(()))\n',b'1\n42\n42\n'),
    ('unit-fixed-generic',
     'export fn answer<T>(value:T,empty:())->T{return value}\n',
     'import {answer} from "./unit-fixed-generic-values"\nprint(answer<i64>(42,()))\n',b'42\n'),
    ('unit-interface',
     'export interface Answer{answer(empty:())->i64}\n'
     'export struct Box implements Answer{value:i64;answer(empty:())->i64{return this.value}}\n'
     'export fn invoke<T:Answer>(value:T,empty:())->i64{return value.answer(empty)}\n',
     'import {Box,invoke} from "./unit-interface-values"\nprint(invoke(Box{value:42},()))\n',b'42\n'),
    ('unit-constructor-inference',
     'export struct Box{value:i64;init(empty:(),value){this.value=value} answer(empty:())->i64{return this.value}}\n',
     'import {Box} from "./unit-constructor-inference-values"\nconst box=Box((),42)\nprint(box.answer(()))\n',b'42\n'),
]

NEGATIVE = [
    ('missing', 'fn answer(empty:())->i64{return 42}\nprint(answer())\n',b'missing required call argument'),
    ('extra', 'fn answer(empty:())->i64{return 42}\nprint(answer((),()))\n',b'too many call arguments'),
    ('integer', 'fn answer(empty:())->i64{return 42}\nprint(answer(0))\n',b'expression cannot satisfy its declared type'),
    ('null', 'fn answer(empty:())->i64{return 42}\nprint(answer(null))\n',b'null requires an exact nullable context'),
    ('erased-callable', 'fn answer(empty:())->i64{return 42}\nconst callback:fn()->i64=answer\n',
     b'callable conversion may only weaken its outer execution contract'),
    ('unresolved-constructor', 'struct Box{value:i64;init(missing){this.value=42}}\nconst box=Box(())\n',
     b'constructor parameter needs a declaration type'),
    ('ctfe-output','fn mark()->(){print(1)}\nfn answer(empty:())->i64{return 42}\n'
     'const result=comptime answer(mark())\n',b'admitted deterministic scalar direct-call closure'),
]
