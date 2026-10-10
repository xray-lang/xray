"""Complete SDK27/72 preimage from the unchanged independent351 layout model."""
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
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,required=True)
 p.add_argument('--output-root',type=Path,required=True);p.add_argument('--facts-directory',type=Path,required=True)
 p.add_argument('--write',action='store_true');a=p.parse_args()
 sys.path.insert(0,str(a.source_root/'scripts'))
 from derive_xir_sdk_abi import prepare
 prepare(a.facts_directory)
 rows=json.loads((a.facts_directory/'EXPECTED.json').read_text())['rows'];assert len(rows)==351
 src=a.source_root/'tests/unit/xir';out=a.output_root/'tests/unit/xir'
 spec=importlib.util.spec_from_file_location('sdk71_model',src/'derive_sdk_current_identity.py')
 model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)
 historical=src/'sdk_previous71_identity_golden.h'
 old=(historical if historical.exists() else src/'sdk_current_identity_golden.h').read_text('utf-8')
 previous=model.current_preimage(rows,71,22,28,26,29);current=model.current_preimage(rows,72,22,28,27,29)
 names=['current_sdk_kat_preimage','current_sdk_kat_file_digest','current_sdk_kat_digest']
 for name,value in zip(names,previous):assert literal(old,name)==value
 wire=len(b'xray:xir-runtime-sdk:v1')+4
 assert previous[0][:wire]==current[0][:wire] and previous[0][wire+8:]==current[0][wire+8:]
 assert len(current[0])==3098 and current[1]==previous[1]
 text='/* Independent current SDK framing and natural Windows C layout facts. */\n'
 for name,value in zip(names,current):
  text+='static const uint8_t '+name+'[] = {\n'
  for i in range(0,len(value),16):text+='    '+','.join('0x%02x'%b for b in value[i:i+16])+',\n'
  text+='};\n'
 if a.write:
  out.mkdir(parents=True,exist_ok=True);(out/'sdk_previous71_identity_golden.h').write_text(previous_header(old),encoding='utf-8',newline='\n')
  (out/'sdk_previous72_identity_golden.h').write_text(text,encoding='utf-8',newline='\n')
 else:assert literal((out/'sdk_previous72_identity_golden.h').read_text('utf-8'),names[0])==current[0] and literal((out/'sdk_previous72_identity_golden.h').read_text('utf-8'),names[1])==current[1] and literal((out/'sdk_previous72_identity_golden.h').read_text('utf-8'),names[2])==current[2]
 print(json.dumps({'wire':27,'semantic':72,'unchanged_independent_abi_fields':351,
  'previous_full_preimage_reproduced':True,'previous_identity':previous[2].hex(),
  'current_identity':current[2].hex(),'preimage_bytes':3098,'production_probe_or_writer_used':False},indent=2))
if __name__=='__main__':main()
