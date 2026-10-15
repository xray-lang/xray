"""Require exact compiler String updates through real cells and library joins."""

CASES = [
 ('append-cow',
  'export fn snapshots()->(string,string){var text="A\\0é";const old=text;'
  'text+="中😀";text+=text;text+="";return (old,text)}\n',
  'import {snapshots} from "./append-cow-values"\nconst appendPair=comptime snapshots();'
  'print(appendPair.0);print(appendPair.1)\n',
  ('A\0é\n'+'A\0é中😀'*2+'\n').encode()),
 ('append-scalars',
  'export fn scalarText()->string{var text="";var integer:i64=-17;text+="${integer}";'
  'text+="|";var truth=true;text+="${truth}";text+="|";var letter=\'Ω\';text+="${letter}";'
  'text+="|";var number:f64=1.5;text+="${number}";text+="|";'
  'var wide:u64=18446744073709551615;text+="${wide}";return text}\n',
  'import {scalarText} from "./append-scalars-values"\nprint(comptime scalarText())\n',
  '-17|true|Ω|1.5|18446744073709551615\n'.encode()),
 ('append-ref-cleanup',
  'fn update(text:ref string){text+="r"}\n'
  'export fn scoped()->(string,string){var text="q";const old=text;update(ref text);'
  'defer {text+="d"};return (old,text)}\n',
  'import {scoped} from "./append-ref-cleanup-values"\nconst appendScoped=comptime scoped();'
  'print(appendScoped.0,appendScoped.1)\n',b'q qr\n'),
 ('append-joins',
  'export fn joins()->(string,string,string){const a:Array<i8>=[-7,12];'
  'const b:Array<f64>=[3.5,-0.0];const c:Array<string>=["A\\0","中"];'
  'return (a.join(":"),b.join(","),c.join("|"))}\n',
  'import {joins} from "./append-joins-values"\nconst appendJoins=comptime joins();'
  'print(appendJoins.0);print(appendJoins.1);print(appendJoins.2)\n',
  '-7:12\n3.5,-0.0\nA\0|中\n'.encode()),
 ('append-projected-ref',
  'class TextBox{value:string;init(value:string){this.value=value}}\n'
  'fn update(text:ref string){text+="r"}\n'
  'export fn projected()->(string,string){const box=TextBox("A\\0é");const old=box.value;'
  'update(ref box.value);return (old,box.value)}\n',
  'import {projected} from "./append-projected-ref-values"\n'
  'const appendProjected=comptime projected();print(appendProjected.0);print(appendProjected.1)\n',
  'A\0é\nA\0ér\n'.encode())]

NEGATIVE = [
 ('immutable','const text="a"\ntext+="b"\nprint(text)\n',
  b'compound assignment requires a mutable binding'),
 ('io','fn text()->string{print(1);var value="a";value+="b";return value}\n'
  'print(comptime text())\n',b'admitted deterministic scalar direct-call closure')]
