#!/usr/bin/env python3
"""Independent complete module-storage wire fields, without a product codec."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct


def words(*values):
    return struct.pack('<'+'I'*len(values), *values)


def blob(value):
    return words(len(value)) + value


def instruction(op, value_type, immediate=0):
    return words(op,value_type,0,0,0,0)+struct.pack('<Q',immediate)+words(0,0)


def function(name, scalar=False):
    count=2 if scalar else 1
    body=blob(name)+words(0,2 if scalar else 0,1,0,count,0,0,count)
    if scalar:
        body+=instruction(2,2,42)
    return body+instruction(33,0)+words(0)


def packet(birth, root, library=False, legacy=False):
    # Linkage, complete function denominator and present declarations.
    body=words(int(library),3,1)
    body+=function(b'birth')+function(b'root')+function(b'main',True)
    # Two modules, no slots/literals, Program root/entry or absent Library entry.
    body+=words(2,0,0,0xffffffff if library else 1,0xffffffff if library else 2)
    body+=blob(b'birth')+words(0,0,0)
    if not legacy: body+=words(birth)
    body+=blob(b'root')+words(1,0,1,0)
    if not legacy: body+=words(root)
    # Function module/export and all seven remaining identity fields.
    body+=words(0,0,0,0,0,0,0,0,0)
    body+=words(1,0,0,0,0,0,0,0,0)
    body+=words(1,1,0,0,0,0,0,0,0)
    # Implementations, generics, types, nominals, interfaces, construction,
    # defaults and absent provenance each retain their mandatory zero field.
    body+=words(0,0,0,0,0,0,0,0)
    prefix=b'XRCHK\0\0\0'+words(29 if legacy else 30,75 if legacy else 76,2,0)
    prefix+=struct.pack('<Q',len(body))
    return prefix+hashlib.sha256(prefix+body).digest()+body


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args()
    checked=(args.source_root/'src/xir/xxir_checked.h').read_text(encoding='utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+30u',checked)
    assert re.search(r'XR_XIR_CHECKED_CONTRACT\s+76u',checked)
    ops=re.findall(r'^XR_XIR_OP\((\w+)',
                   (args.source_root/'src/xir/xxir_ops.def').read_text(encoding='utf-8'),re.M)
    assert ops[1]=='CONST_INT' and ops[32]=='RETURN'
    vectors=[('birth76_program',packet(1,0)),('birth76_fresh',packet(0,0)),
             ('birth76_root_rejected',packet(1,1)),('birth76_unknown_rejected',packet(2,0)),
             ('birth76_library_rejected',packet(1,0,True)),
             ('birth76_library_fresh',packet(0,0,True)),
             ('birth75_retired',packet(0,0,legacy=True))]
    lines=['/*',' * xray - Lightweight typed scripting with native concurrency',
           ' * https://www.xray-lang.org',' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>',
           ' * Licensed under the MIT License',' *',
           ' * birth_storage76_golden.h - Independent complete module storage packets',
           ' */','#ifndef BIRTH_STORAGE76_GOLDEN_H','#define BIRTH_STORAGE76_GOLDEN_H',
           '#include <stdint.h>',
           '_Static_assert(XR_XIR_CHECKED_SCHEMA==30u && XR_XIR_CHECKED_CONTRACT==76u &&',
           '    XR_XIR_CONST_INT==2 && XR_XIR_RETURN==33 && XR_XIR_I64==2 && XR_XIR_UNIT==0 &&',
           '    XR_XIR_MODULE_FRESH_STORAGE==0 && XR_XIR_MODULE_BIRTH_STORAGE==1,',
           '    "Independent module storage wire field identities changed");']
    for name,value in vectors:
        lines.append('static const uint8_t '+name+'[] = {')
        lines.extend('    '+', '.join('0x%02x'%v for v in value[i:i+12])+','
                     for i in range(0,len(value),12))
        lines.append('};')
    lines.append('#endif // BIRTH_STORAGE76_GOLDEN_H')
    text='\n'.join(lines)+'\n'
    target=args.source_root/'tests/unit/xir/compile_owner/birth_storage76_golden.h'
    if args.write: target.write_text(text,encoding='utf-8',newline='\n')
    else: assert target.read_text(encoding='utf-8')==text
    print(json.dumps(dict(product_codec_used=False,status='INDEPENDENT_FIELDS_ONLY_NO_C_RUN',
                         vectors=[dict(name=n,bytes=len(v),sha256=hashlib.sha256(v).hexdigest())
                                  for n,v in vectors]),indent=2))


if __name__=='__main__':
    main()
