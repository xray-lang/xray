"""Complete SDK27/72 bytes from the original immutable 351-field packets."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,sys
def literal(text,symbol):
 m=re.search(r'static const uint8_t '+symbol+r'\[\] = \{(.*?)\};',text,re.S);assert m
 return bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',m[1]))
def previous_header(text):
 guard='SDK_PREVIOUS71_IDENTITY_GOLDEN_H'
 if '#ifndef '+guard in text:return text
 title=('/*\n * xray - Lightweight typed scripting with native concurrency\n'
  ' * https://www.xray-lang.org\n *\n'
  ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n'
  ' * Licensed under the MIT License\n *\n'
  ' * sdk_previous71_identity_golden.h - Historical complete SDK identity fields\n *\n'
  ' * KEY CONCEPT:\n *   Preserved bytes establish historical framing and fail-closed refusal.\n */\n')
 return title+'#ifndef '+guard+'\n#define '+guard+'\n#include <stdint.h>\n'+text+'\n#endif // '+guard+'\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--output-root',type=Path,required=True)
    parser.add_argument('--facts-directory',type=Path,required=True)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    sys.dont_write_bytecode=True
    src=args.source_root/'tests/unit/xir'
    out=args.output_root/'tests/unit/xir'
    sys.path.insert(0,str(src))
    from sdk_identity_model import c_array, frame, retired_packets
    packets=retired_packets(src)
    def original(name):
        matches=[packet for packet in packets if any(
            ref['label']=='P10' and ref['path']=='tests/unit/xir/'+name
            for ref in packet['provenance'])]
        assert len(matches)==1
        return matches[0]
    prior=original('sdk_previous71_identity_golden.h')
    current=original('sdk_previous72_identity_golden.h')
    rows=current['manifest']['abi_measurements']
    assert len(rows)==351 and rows==prior['manifest']['abi_measurements']
    assert (prior['manifest']['wire'],prior['manifest']['semantic'])==(26,71)
    assert (current['manifest']['wire'],current['manifest']['semantic'])==(27,72)
    previous=[frame(prior['manifest']),bytes.fromhex(prior['file_digest_hex']),bytes.fromhex(prior['digest_hex'])]
    values=[frame(current['manifest']),bytes.fromhex(current['file_digest_hex']),bytes.fromhex(current['digest_hex'])]
    assert all(hashlib.sha256(value[0]).digest()==value[2] for value in (previous,values))
    names=['current_sdk_kat_preimage','current_sdk_kat_file_digest','current_sdk_kat_digest']
    old=(src/'sdk_previous71_identity_golden.h').read_text(encoding='utf-8')
    for name,value in zip(names,previous):
        assert literal(old,name)==value
    wire=len(b'xray:xir-runtime-sdk:v1')+4
    assert previous[0][:wire]==values[0][:wire] and previous[0][wire+8:]==values[0][wire+8:]
    assert len(values[0])==3098 and values[1]==previous[1]
    args.facts_directory.mkdir(parents=True,exist_ok=False)
    (args.facts_directory/'EXPECTED.json').write_text(json.dumps({
        'recipe':'complete-retired-sdk27-72-fields',
        'rows':rows,
        'source_git_blob':current['git_blob'],
        'basis':'Complete original immutable packet, independent of the current layout table',
    },indent=2)+'\n',encoding='utf-8',newline='\n')
    text='/* Independent complete historical SDK framing. */\n'
    text+=''.join(c_array(name,value) for name,value in zip(names,values))
    if args.write:
        out.mkdir(parents=True,exist_ok=True)
        (out/'sdk_previous71_identity_golden.h').write_text(previous_header(old),encoding='utf-8',newline='\n')
        (out/'sdk_previous72_identity_golden.h').write_text(text,encoding='utf-8',newline='\n')
    else:
        checked=(out/'sdk_previous72_identity_golden.h').read_text(encoding='utf-8')
        for name,value in zip(names,values):
            assert literal(checked,name)==value
    print(json.dumps({
        'wire':27,'semantic':72,'unchanged_independent_abi_fields':351,
        'previous_full_preimage_reproduced':True,'previous_identity':previous[2].hex(),
        'current_identity':values[2].hex(),'preimage_bytes':3098,
        'production_probe_or_writer_used':False,'current_layout_table_used':False,
    },indent=2))
if __name__=='__main__':main()
