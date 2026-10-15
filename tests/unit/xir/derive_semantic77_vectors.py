"""Derive unchanged positive fields under closed text semantics, retaining old packets."""
from pathlib import Path
import argparse,hashlib,re,struct
from derive_construction72_vectors import header

PINS = {
    'xir_assert_panics76_golden.h':'b92b2d724a4759b70ed7113a58c1fea85902f5ec9ee40b81d7c6cafbf9113d91',
    'xir_checked_range76_golden.h':'35297c2b814f9fbdc1d8a3bc03c177f55cec01751a317d97e52c19a43b4a261b',
    'xir_checked_scalar76_golden.h':'d101a7f6c2bdfbced6f3e594b71ad37d4f92c39fc961678e4b9b9219eacfa0cf',
    'xir_error_marker76_golden.h':'bb861dbdd35a672ed9df3d16cefe06cd23066fdee18fc8a9428fda7cbdb41458',
    'xir_generic_method76_golden.h':'738389b27c83d894ae3b5da1d3b172ef7c2eb9e2e8377c4053e55490ae7de872',
    'xir_hash76_golden.h':'6c15e139def2f144513d0a567b4c999a6a3b43d7f1c07514154eb199c8d6d3e3',
    'xir_nested_nullable76_golden.h':'f91e7ab56c285e3631a4655586c8a5ed5ddd80b044c68fb257a26d74fef90495',
    'xir_nullable76_golden.h':'a8d90d4d8f411a8b3cc64ddf597399b3b78ada1794479f9196df6d6738f82737',
    'xir_rune76_golden.h':'9002539cdaeb62a3c67b5366497ca277068143252a08a5102aa1e764d2ecb5bc',
    'compile_owner/checked_sizing76_golden.h':'292aa3eb776ade40895301ff7a8330e714898522adb5bafc307d18de3c673573',
    'unit_values/unit_wire_golden.h':'2d127a7982b10cb2f2e9a0898692f532d639b40795687e9ed16ce3446d586ff7',
}
ARRAY=re.compile(r'static const uint8_t (\w+)\[\] = \{(.*?)\};',re.S)
def octets(match):return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[2]))
def render(name,data):
    return 'static const uint8_t '+name+'[] = {\n'+''.join('    '+', '.join('0x%02x'%b for b in data[i:i+12])+',\n' for i in range(0,len(data),12))+'};'

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    root=Path(__file__).resolve().parent;retired=[];count=0
    for rel,pin in PINS.items():
        source=root/rel;text=source.read_text(encoding='utf-8');assert hashlib.sha256(text.encode()).hexdigest()==pin,rel
        replacements={};digests={}
        for match in ARRAY.finditer(text):
            data=octets(match)
            if data[:8]!=b'XRCHK\0\0\0' or struct.unpack_from('<II',data,8)!=(29,76):continue
            assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
            assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]
            retired.append(('local_previous76_'+match[1],data))
            prefix=data[:12]+struct.pack('<I',77)+data[16:32]
            digest=hashlib.sha256(prefix+data[64:]).digest()
            replacements[match[1]]=prefix+digest+data[64:];digests[data[32:64]]=digest;count+=1
            assert replacements[match[1]][64:]==data[64:]
        def row(match):
            old=octets(match);return render(match[1].replace('76','77'),replacements.get(match[1],digests.get(old,old)))
        output=ARRAY.sub(row,text)
        output=re.sub(r'\b[A-Za-z_]\w*76\w*\b',lambda m:m[0].replace('76','77'),output)
        output=output.replace('CONTRACT == 76u','CONTRACT == 77u')
        target=source.with_name(source.name.replace('76','77')) if '76' in source.name else source.with_name('unit_wire77_golden.h')
        if args.write:target.write_text(output,encoding='utf-8',newline='\n')
        else:assert target.read_text(encoding='utf-8')==output,target
    assert count==16 and len(PINS)==11
    guard='XIR_LOCAL_PREVIOUS76_GOLDEN_H';target=root/'xir_local_previous76_golden.h'
    lines=['/* Complete pinned prior packets; no compiler writer supplies these bytes. */',
           '#ifndef '+guard,'#define '+guard,'#include <stdint.h>','#include <stddef.h>']
    lines.extend(render(name,data) for name,data in retired)
    lines+=['typedef struct LocalPrevious76Vector { const uint8_t *bytes; size_t length; } LocalPrevious76Vector;',
            'static const LocalPrevious76Vector local_previous76_vectors[] = {']
    lines+=['    {'+name+',sizeof('+name+')},' for name,_ in retired]
    output='\n'.join(lines+['};','#endif // '+guard,''])
    if args.write:target.write_text(output,encoding='utf-8',newline='\n')
    else:assert target.read_text(encoding='utf-8')==output
    print('Independent semantic77:16 complete current field bodies,16 complete old76 packets,11 pinned headers; no product writer.')
if __name__=='__main__':main()
