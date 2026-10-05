"""Named complete Tuple and historical-family oracles for current Checked25/65."""
import argparse,hashlib,json,struct
from pathlib import Path
from derive_tuple63_vectors import words,operation
from derive_semantic65_packets import fixed_packet,frame
from derive_role_nullable65_packets import current_packet as nullable_packet
DIRECTORY=Path(__file__).resolve().parent

def tuple_packet(semantic=65):
    assert semantic in (64,65)
    ops=[operation(2,2,immediate=73),operation(143,256,right=2),operation(144,0,left=2),
         operation(144,3,left=2,immediate=2),operation(144,2,left=2,immediate=1),operation(33,left=5)]
    function=words(5)+b'tuple'+words(1,3,2,1,0,6,0,0,6)+b''.join(ops)+words(2,1,0)
    body=words(0,1,0)+function+words(0,1,0,0,6,0,3,0,2,3,0,0)
    return frame(body,semantic)

def current_family(row,previous63,semantic=65):
    assert struct.unpack_from('<4I',previous63,8)==(24,63,2,0)
    assert hashlib.sha256(previous63).hexdigest()==row['current63_sha256']
    name=row['name']
    if name=='tuple_24_62':return tuple_packet(semantic)
    if name in ('nullable_none_golden','nullable_some_golden'):return nullable_packet(previous63,semantic)
    mapping={'sizing_golden':'sizing65_golden','sizing_embedded_golden':'sizing65_embedded_golden',
             'sizing_empty_golden':'sizing65_empty_golden','assert_condition_golden':'assert_condition65_golden',
             'assert_equal_golden':'assert_equal65_golden','assert_panics_golden':'assert_panics65_golden',
             'checked_scalar60_golden':'checked_scalar65_golden','defaults_invoke_golden':'defaults65_invoke_golden',
             'generic_method_golden':'generic_method65_golden','nested_nullable_golden':'nested_nullable65_golden',
             'rune_checked_golden':'rune65_golden'}
    mapping.update({'defaults_golden_'+str(i):'defaults65_golden_'+str(i) for i in range(7,11)})
    mapping.update({'library_string_'+n:'library_string65_'+n for n in ('alpha','beta','nul','empty','long','equal','unused')})
    assert name in mapping,'unpriced or unnamed historical family: '+name
    return fixed_packet({'name':mapping[name]},semantic)

def literal():
    lines=['/* Independently encoded complete Tuple packets; current65 and rejected prior64. */']
    for semantic in (64,65):
        data=tuple_packet(semantic);lines+=['static const unsigned char tuple_25_'+str(semantic)+'[]={']
        lines+=['    '+','.join('0x%02x'%b for b in data[i:i+16])+',' for i in range(0,len(data),16)]
        lines+=['};']
    return '\n'.join(lines)+'\n'

def verify_literals():
    assert (DIRECTORY/'tuple_25_65.inc.c').read_text(encoding='utf-8')==literal()

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    if args.write:(DIRECTORY/'tuple_25_65.inc.c').write_text(literal(),encoding='utf-8',newline='\n')
    verify_literals()
    print('Independent Tuple current25/65 and complete previous25/64 oracles PASS',len(tuple_packet()),hashlib.sha256(tuple_packet()).hexdigest())
if __name__=='__main__':main()
