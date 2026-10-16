"""Derive numeric oracles with independent 200-digit arithmetic (mpmath 1.3).

The product kernel, system libm and generated C are never executed or imported.
This offline derivation needs mpmath; ordinary C tests consume the retained rows.
"""
from pathlib import Path
import argparse, hashlib, json, struct
import mpmath as mp

HERE=Path(__file__).resolve().parent
mp.mp.dps=200
bits=lambda value:struct.unpack('<Q',struct.pack('<d',float(value)))[0]
NAN=0x7ff8000000000000
SIGN=0x8000000000000000
rows=[]

def add(name,left,right,expected,integer=False,status='XR_XIR_RUN_OK',ulp=4):
    first=bits(left);second=0 if right is None else bits(right)
    if integer:result=int(expected)&((1<<64)-1);ulp=0
    else:result=NAN if mp.isnan(expected) else bits(expected)
    rows.append((name,first,second,result,status,ulp))

def derive():
    functions={'ABS':abs,'SQRT':mp.sqrt,'SIN':mp.sin,'COS':mp.cos,'TAN':mp.tan,
        'ASIN':mp.asin,'ACOS':mp.acos,'ATAN':mp.atan,'LOG':mp.log,'LOG10':mp.log10,
        'LOG2':lambda x:mp.log(x,2),'EXP':mp.exp,'SINH':mp.sinh,'COSH':mp.cosh,
        'TANH':mp.tanh,'CBRT':lambda x:mp.sign(x)*mp.root(abs(x),3),
        'LOG1P':mp.log1p,'EXPM1':mp.expm1}
    samples=[-8.0,-2.5,-1.0,-0.5,0.0,0.125,0.5,1.0,2.0,8.0,100.0]
    for name,function in functions.items():
        for value in samples:
            try:result=function(mp.mpf(value))
            except (ValueError,ZeroDivisionError):result=mp.nan
            if isinstance(result,mp.mpc):result=mp.nan if result.imag else result.real
            add(name,value,None,result)
    pairs={'POW':[(2.0,3.0),(-2.0,3.0),(-2.0,-3.0),(0.25,0.5),(4.0,1.5),(-1.0,0.5)],
        'ATAN2':[(1.0,1.0),(1.0,-1.0),(-1.0,-1.0),(2.0,0.5),(-3.0,7.0),(0.0,1.0)],
        'HYPOT':[(3.0,4.0),(-3.0,4.0),(1e200,1e200),(1e-200,1e-200),(0.5,0.125),(0.0,0.0)],
        'FMOD':[(5.5,2.0),(-5.5,2.0),(5.5,-2.0),(-5.5,-2.0),(0.125,0.5),(1.0,0.0)]}
    for name,values in pairs.items():
        for left,right in values:
            a,b=mp.mpf(left),mp.mpf(right)
            try:
                result=mp.power(a,b) if name=='POW' else mp.atan2(a,b) if name=='ATAN2' else \
                    mp.sqrt(a*a+b*b) if name=='HYPOT' else a-mp.sign(a/b)*mp.floor(abs(a/b))*b
            except (ValueError,ZeroDivisionError):result=mp.nan
            if isinstance(result,mp.mpc):result=mp.nan if result.imag else result.real
            add(name,left,right,result)
    rounding={'FLOOR':mp.floor,'CEIL':mp.ceil,'ROUND':lambda x:mp.floor(x+mp.mpf('0.5')) if x>=0 else mp.ceil(x-mp.mpf('0.5')),'TRUNC':lambda x:mp.sign(x)*mp.floor(abs(x))}
    inputs=[-2.5,-1.5,-0.5,0.0,0.5,1.5,2.5,4503599627370497.0,-9223372036854775808.0,9223372036854774784.0,9223372036854775808.0,-9223372036854777856.0]
    for name,function in rounding.items():
        for value in inputs:
            result=int(function(mp.mpf(value)));valid=-(1<<63)<=result<(1<<63)
            add(name,value,None,result if valid else 0,integer=True,status='XR_XIR_RUN_OK' if valid else 'XR_XIR_RUN_NUMERIC_RANGE')
        for value in [float('nan'),float('inf'),-float('inf')]:add(name,value,None,0,integer=True,status='XR_XIR_RUN_NUMERIC_RANGE')
    for name in functions:
        add(name,float('nan'),None,mp.nan,ulp=0)
    for name in ['SQRT','SIN','TAN','ASIN','ATAN','SINH','TANH','CBRT','LOG1P','EXPM1']:
        rows.append((name,SIGN,0,SIGN,'XR_XIR_RUN_OK',0))
    rows.extend([('ABS',SIGN,0,0,'XR_XIR_RUN_OK',0),
        ('HYPOT',SIGN,SIGN,0,'XR_XIR_RUN_OK',0),
        ('FMOD',SIGN,bits(3.0),SIGN,'XR_XIR_RUN_OK',0),
        ('POW',bits(-1.0),bits(0.5),NAN,'XR_XIR_RUN_OK',0)])
    add('LOG1P',1e-20,None,mp.log1p(mp.mpf(1e-20)))
    add('EXPM1',1e-20,None,mp.expm1(mp.mpf(1e-20)))
    for name in ['SIN','COS','TAN']:add(name,1e100,None,functions[name](mp.mpf(1e100)))
    assert {row[0] for row in rows}==set(functions)|set(rounding)|set(pairs)

def render():
    derive()
    text='/* Independent 200-digit reference rows; system libm and product writer unused. */\n'
    text+='static const MathOracle math_oracles[] = {\n'
    for name,left,right,result,status,ulp in rows:
        text+=f'    {{XR_XIR_MATH_{name},UINT64_C(0x{left:016x}),UINT64_C(0x{right:016x}),UINT64_C(0x{result:016x}),{status},{ulp}}},\n'
    text+='};\n'
    return text

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    text=render();path=HERE/'numeric_vectors.h'
    if args.write:path.write_text(text,encoding='utf-8',newline='\n')
    else:assert path.read_text(encoding='utf-8')==text
    print(json.dumps(dict(rows=len(rows),pure_operations=len({row[0] for row in rows}),precision_digits=mp.mp.dps,mpmath_version=mp.__version__,system_libm_used=False,product_writer_used=False,sha256=hashlib.sha256(text.encode()).hexdigest())))
if __name__=='__main__':main()
