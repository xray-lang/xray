"""Independent required nominal compiler values and exact byte oracles."""

CASES = [('public',
  'export struct Pair{value:i8;text:string}\nexport fn make()->Pair{return Pair{value:7,text:"A\\0中"}}\n',
  'import {make} from "./public-values"\nconst p=comptime make()\nprint(p.value,p.text)\n',
  b'7 A\x00\xe4\xb8\xad\n'),
 ('generic',
  'export struct Box<T>{value:T;marker:i8}\n'
  'export fn make()->Box<string>{return Box<string>{value:"é\\0",marker:3}}\n',
  'import {make} from "./generic-values"\nconst p=comptime make()\nprint(p.value,p.marker)\n',
  b'\xc3\xa9\x00 3\n'),
 ('enum',
  'export struct Pair{value:i8;text:string}\n'
  'export enum Choice{Empty,Some{box:Pair}}\n'
  'export fn make()->Choice{return Choice.Some{box:Pair{value:7,text:"a\\0b"}}}\n',
  'import {make,Choice} from "./enum-values"\n'
  'const p=comptime make()\n'
  'match(p){Choice.Empty->print(0),Choice.Some{box}->print(box.value,box.text)}\n',
  b'7 a\x00b\n'),
 ('fields',
  'struct Box{xs:Array<Array<i8>>;text:string}\n'
  'export fn inspect()->bool{var b=Box{xs:[[1,2]],text:"old"};const '
  'old=b;b.xs[0].push(7);b.text="new";return len(old.xs[0])==2 && old.text=="old" && len(b.xs[0])==3 && '
  'b.xs[0][2]==7 && b.text=="new"}\n',
  'import {inspect} from "./fields-values"\nprint(comptime inspect())\n',
  b'true\n'),
 ('constructor',
  'export struct Row{items:Array<i8>=[1];stamp:i8;init(){this.items.push(7);this.stamp=2}}\n'
  'export fn make()->Row{return Row()}\n',
  'import {make} from "./constructor-values"\n'
  'const p=comptime make()\n'
  'print(p.items[0],p.items[1],p.stamp)\n',
  b'1 7 2\n'),
 ('spread',
  'export struct Box<T>{value:T;marker:i8}\n'
  'export fn make()->Box<string>{const old=Box<string>{value:"a\\0",marker:2};return '
  'Box<string>{...old,marker:7}}\n',
  'import {make} from "./spread-values"\nconst p=comptime make()\nprint(p.value,p.marker)\n',
  b'a\x00 7\n'),
 ('private-inside',
  'struct Secret{private value:i8;static make()->Secret{return Secret{value:7}};get()->i8{return '
  'this.value}}\n'
  'export fn inspect()->i8{const b=Secret.make();return b.get()}\n',
  'import {inspect} from "./private-inside-values"\nprint(comptime inspect())\n',
  b'7\n'),
 ('private-result',
  'export struct Secret{private value:i8;static make()->Secret{return Secret{value:7}};get()->i8{return '
  'this.value}}\n'
  'export fn make()->Secret{return Secret.make()}\n',
  'import {make} from "./private-result-values"\nconst p=comptime make()\nprint(p.get())\n',
  b'7\n'),
 ('unit-field',
  'export struct Pair{value:i8;empty:()}\nexport fn make()->Pair{return Pair{value:7,empty:()}}\n',
  'import {make} from "./unit-field-values"\nconst p=comptime make()\nprint(p.value)\n',
  b'7\n')]
