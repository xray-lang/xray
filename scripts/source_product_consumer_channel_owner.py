#!/usr/bin/env python3
"""Verify unchanged Channel inputs and encode detached diagnostic obligations."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'
CASES = [
    ('channel_handles', '0a411e41caa340ebbe965bf1f258b4685bb02f4b091a4c4bb17ecbfeabca4fd1', 42),
    ('channel_module_types', '7311d03201c5d2d3f2851b34bf35a94bce372dca686ae6c8ad470fec7e3ece22', 0),
    ('channel_constructor_minimal', 'adec2c9dda12160deb17e65383bbd24c2e10f0c55f48cda1acd575e2ac49b7ff', 42),
    ('channel_nullable_minimal', '25c6877bb53e669206c730e442ad36458b6fcbe52eeb88bf943a16ff505279ae', 0),
]


def header():
    old = json.loads((ROOT/'source_channel_admission_obligations.json').read_text())
    lines = ['/* Every Channel Source positive retains expected admission success. */',
             'typedef struct ChannelOwnerCase { const char *name; int64_t runtime_expected; } ChannelOwnerCase;',
             'static const ChannelOwnerCase channel_owner_cases[] = {']
    for name, digest, expected in CASES:
        row = next(x for x in old['cases'] if x['name'] == name)
        raw = (ROOT/'channel_admission'/name/'root.xr').read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest or row['fixture_sha256'] != digest:
            raise ValueError(f'original Channel fixture changed: {name}')
        if row['expected_i64'] != expected or row['expected_probe_exit'] != 0:
            raise ValueError(f'original Channel positive expectation changed: {name}')
        if hashlib.sha256(raw[:row['original_bytes']]).hexdigest() != row['original_sha256']:
            raise ValueError(f'original Channel prefix changed: {name}')
        lines.append(f'    {{"{name}", {expected}}},')
    lines += ['};', '']
    return '\n'.join(lines)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('write','check'));args=parser.parse_args()
    path=ROOT/'channel_owner_cases.h';text=header()
    if args.mode=='write':path.write_text(text,encoding='utf8',newline='\n')
    elif path.read_text(encoding='utf8')!=text:raise ValueError('Channel owner case table differs')
    print('4unchanged Channel positives;diagnostic-present/absent;failure cleanup and occupied output are distinct duties')
