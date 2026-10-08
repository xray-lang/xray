#!/usr/bin/env python3
"""Encode independent module initialization graphs with explicit execution oracles."""
import argparse
import hashlib
import json
from pathlib import Path
import source_product_consumer_module_graph as graph

DIR = Path(__file__).resolve().parents[1] / 'tests/unit/xir/product_consumers'


def artifacts():
    rows = [(name, model) for name, model, _ in graph.cases()[:3]]
    m = graph.model(); m['names'] = [b'base', b'right', b'left', b'main']
    rows.append(('lexical_tie_break', m))
    orders = [[0,1,2,3], [0,1,2,3], [3,1,2,0], [0,2,1,3]]
    lines = ['/* Complete independent inputs and fixed initializer-order oracles. */']
    manifest = dict(schema=1, cases=[])
    for i, ((name, model), order) in enumerate(zip(rows, orders)):
        packet, _ = graph.encode(model)
        lines.append(f'static const uint8_t module_execution_packet_{i}[] = {{')
        for offset in range(0, len(packet), 32):
            lines.append('    '+','.join(f'0x{x:02x}' for x in packet[offset:offset+32])+',')
        lines.append('};')
        manifest['cases'].append(dict(name=name, bytes=len(packet), sha256=hashlib.sha256(packet).hexdigest(), order=order, entry=4, result=42))
    lines += ['typedef struct ModuleExecutionCase {',
              '    const char *name; const uint8_t *bytes; size_t length; uint32_t order[4];',
              '} ModuleExecutionCase;', 'static const ModuleExecutionCase module_execution_cases[] = {']
    for i, row in enumerate(manifest['cases']):
        lines.append('    {"%s",module_execution_packet_%d,sizeof(module_execution_packet_%d),{%s}},' % (row['name'],i,i,','.join(map(str,row['order']))))
    lines += ['};', '']
    return '\n'.join(lines), manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('write','check'))
    args = parser.parse_args(); header, manifest = artifacts()
    for path, text in {DIR/'module_execution_cases.h':header, DIR/'module_execution_cases.json':json.dumps(manifest,indent=2)+'\n'}.items():
        if args.mode == 'write': path.write_text(text,encoding='utf8',newline='\n')
        elif path.read_text(encoding='utf8') != text: raise ValueError(f'independent module execution fixture differs: {path}')
    print('Module execution: four complete graphs, fixed dependency/lexical order, entry returns 42')
