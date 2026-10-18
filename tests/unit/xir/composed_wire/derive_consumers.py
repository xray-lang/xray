"""Derive current Record and structural consumers from complete original fields."""
from pathlib import Path
import argparse
import hashlib
import json
import sys

sys.dont_write_bytecode = True
from model import decode, encode, compose
from derive import original, array, output

ROOT = Path(__file__).resolve().parent


def build(write=False):
    registry = json.loads((ROOT / 'inputs.json').read_text('utf-8'))
    facts = []
    for family, count in (('record', 7), ('structural', 12)):
        rows = [row for row in registry if row['label'] == 'P10' and row['symbol'].startswith(family + '81_')]
        assert len(rows) == count, (family, len(rows))
        text = '/* Complete independent current peers; all preceding packets stay immutable. */\n'
        for row in rows:
            old = original(row)
            fields, _ = decode(old, 'P10')
            assert encode(fields, 'P10')[0] == old
            current, _ = encode(compose(fields, 'P10'))
            assert encode(decode(current, 'current')[0])[0] == current
            name = row['symbol'].replace(family + '81_', family + '90_')
            text += array(name, current)
            facts.append(dict(name=name, original_symbol=row['symbol'], original_sha256=row['sha256'],
                              current_bytes=len(current), current_sha256=hashlib.sha256(current).hexdigest()))
        assert len(text.splitlines()) <= 3000
        output(ROOT / (family + '90.inc.c'), text, write)
    output(ROOT / 'consumer_model_facts.json', json.dumps(facts, indent=2) + '\n', write)
    return dict(current_complete_packets=len(facts), record_packets=7, structural_packets=12,
                original_fields_round_tripped=True, product_writer_used=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    print(json.dumps(build(args.write), indent=2))
