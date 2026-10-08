#!/usr/bin/env python3
"""Preserve legal class-parent chains and distinguish masked negative cases."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers/parent_source'
FOOT = 'export fn consumerAnswer() -> i64 { return answer() }\nfn main() -> i64 { return 0 }\n'
FIELDS = 'class Root { value: i64; text: string; constructor(value: i64, text: string) { this.value = value; this.text = text } }\n'
ANSWER = 'fn answer() -> i64 { const value = Root(40, "x"); const alias = value; alias.value += 2; assert(value.text == "x"); return value.value }\n'


def cases():
    def row(name, body, expected=0, fields=0, final=0, control=0):
        return dict(name=name, expected=expected, fields=fields, final=final, control=control, source=body+FOOT)
    def declarations(name, body, expected=0):
        return row(name, body+'fn answer() -> i64 { return 42 }\n', expected)
    return [
        row('open_root_empty', 'class Root {}\nfn answer() -> i64 { const value = Root(); return 42 }\n', control=1),
        row('open_root_fields', FIELDS+ANSWER, fields=2, control=1),
        row('final_root_fields', 'final '+FIELDS+ANSWER, fields=2, final=1, control=1),
        declarations('parent_chain_fields', 'class Base {}\nclass Middle extends Base { value: i64 = 40 }\nclass Leaf extends Middle { text: string = "x" }\n'),
        declarations('parent_chain_empty', 'class Base {}\nclass Middle extends Base {}\nclass Leaf extends Middle {}\n'),
        declarations('self_parent', 'class Root extends Root {}\n', 3),
        declarations('cycle', 'class Base extends Leaf {}\nclass Middle extends Base {}\nclass Leaf extends Middle {}\n', 3),
        # The grammar requires an Identifier here; the i64 keyword fails parsing.
        declarations('scalar_parent', 'class Root extends i64 {}\n', 1),
        declarations('record_parent', 'struct Base {}\nclass Root extends Base {}\n', 3),
        declarations('final_parent', 'final class Base {}\nclass Root extends Base {}\n', 3),
        declarations('missing_parent', 'class Root extends Missing {}\n', 3),
    ]


def artifacts():
    result = {}; manifest = dict(schema=1, cases=[])
    lines = ['/* Legal parent chains stay positive; blanket inheritance rejection does not qualify negatives. */',
             'typedef struct ParentSourceCase { const char *name; XrXirStatus expected; unsigned fields, final, control; } ParentSourceCase;',
             'static const ParentSourceCase parent_source_cases[] = {']
    for case in cases():
        result[ROOT/case['name']/'root.xr'] = case['source']
        manifest['cases'].append({k:v for k,v in case.items() if k!='source'} | dict(
            sha256=hashlib.sha256(case['source'].encode()).hexdigest(), runtime_expected=42 if not case['expected'] else None))
        lines.append('    {"%s",%u,%u,%u,%u},' % (case['name'],case['expected'],case['fields'],case['final'],case['control']))
    lines += ['};', ''];manifest['runtime_cases']=['open_root_empty','open_root_fields']
    manifest['boundary']='Source witnesses of legacy CoreIR parent-graph duties,not literal old fixtures. Five positive expectations remainOK;blanket unsupported inheritance masks five negative reasons;the scalar keyword case fails parsing and does not prove nominal-parent checking. Root class controls and final flag do not prove parent behavior. No retirement.'
    result[ROOT/'cases.h']='\n'.join(lines);result[ROOT/'manifest.json']=json.dumps(manifest,indent=2)+'\n'
    return result


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('write','check'));args=parser.parse_args()
    for path,text in artifacts().items():
        if args.mode=='write':path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8')!=text:raise ValueError(f'parent source input differs: {path}')
    print('Parent source:5positive/6negative cases;2root runtime controls expect42;parent duties remainOPEN')
