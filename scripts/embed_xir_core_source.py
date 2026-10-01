"""Embed the one compiler-owned intrinsic declaration source without rewriting it."""
import argparse
from pathlib import Path

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('source',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    data=args.source.read_bytes()
    data.decode('utf-8')
    if not data or b'\0' in data:
        raise SystemExit('Core declaration source must be nonempty UTF-8 without NUL')
    rows=[', '.join(f'0x{x:02x}' for x in (data+b'\0')[i:i+20]) for i in range(0,len(data)+1,20)]
    text='/* Generated compiler-owned declaration bytes. */\n'
    text+='static const char xir_core_declaration_source[] = {\n    '+',\n    '.join(rows)+'\n};\n'
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(text,encoding='utf-8',newline='\n')

if __name__=='__main__':
    main()
