"""Derive Iterator packets from complete named fields without a product writer."""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct

def words(*items):
    return struct.pack('<' + 'I' * len(items), *items)

def packet(kind=11, element=2, second=3, contract=84, second_kind=11):
    name = b'iteratorTypes'
    function = words(len(name)) + name + words(2, 256, 257, 0)
    function += words(1, 0, 1, 0, 0)
    function += words(1, 33, 0, 0, 0, 0, 0) + struct.pack('<q', 0) + words(0, 0)
    function += words(0)
    body = words(0, 1, 0) + function
    body += words(0, 2, 0, 0, kind, 0, element, second_kind, 0, second, 0, 0, 0)
    prefix = b'XRCHK\0\0\0' + struct.pack('<IIIIQ', 28, contract, 2, 0, len(body))
    return prefix + hashlib.sha256(prefix + body).digest() + body

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('iterator_header', here / 'derive_construction72_vectors.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    rows = [('iterator_types_golden', packet()), ('iterator_types_unit', packet(element=0)),
            ('iterator_types_cycle', packet(element=256)),
            ('iterator_types_unknown', packet(kind=0xffffffff)),
            ('iterator_types_duplicate', packet(second=2))]
    previous = [('iterator_types_previous73_golden', packet(kind=9, contract=73, second_kind=9)),
                ('iterator_types_previous73_unit', packet(element=0, kind=9, contract=73, second_kind=9)),
                ('iterator_types_previous73_cycle', packet(element=256, kind=9, contract=73, second_kind=9)),
                ('iterator_types_previous73_unknown', packet(kind=0xffffffff, contract=73, second_kind=9)),
                ('iterator_types_previous73_duplicate', packet(second=2, kind=9, contract=73, second_kind=9))]
    helper.header(here / 'xir_iterator_types_previous73_golden.h', 'XIR_ITERATOR_TYPES_PREVIOUS73_GOLDEN_H', previous, False)
    previous74=[(n.replace('iterator_types_','iterator_types_previous74_'),packet(kind=9,element=e,second=s,contract=74,second_kind=9)) for n,e,s in [('iterator_types_golden',2,3),('iterator_types_unit',0,3),('iterator_types_cycle',256,3),('iterator_types_unknown',2,3),('iterator_types_duplicate',2,2)]]
    previous74[3]=('iterator_types_previous74_unknown',packet(kind=0xffffffff,contract=74,second_kind=9))
    helper.header(here / 'xir_iterator_types_previous74_golden.h','XIR_ITERATOR_TYPES_PREVIOUS74_GOLDEN_H',previous74,False)
    helper.header(here / 'xir_iterator_types_golden.h', 'XIR_ITERATOR_TYPES_GOLDEN_H', rows, args.write)
    print(json.dumps({'writer_used': False, 'packets': [{'name': n, 'bytes': len(b),
          'sha256': hashlib.sha256(b).hexdigest()} for n, b in rows]}, indent=2))

if __name__ == '__main__':
    main()
