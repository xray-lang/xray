"""Independent full-field Library/default 27/72 vectors.
The fixed models preserve historical declarations and body semantics. Historical
26/71 literals are complete comparison inputs, never the current writer oracle.
"""
import argparse, hashlib, json, re, struct
from pathlib import Path
HERE=Path(__file__).resolve().parent
MODEL_SHA='e70c3005ea46a0c2fea0568985f8608b449b0d43398360a709e75328bf78a5f3'
def words(*values):
    return struct.pack('<' + 'I' * len(values), *values)
def counted(text):
    raw = text.encode('utf-8')
    return words(len(raw)) + raw
def vector(values):
    return words(len(values)) + words(*values)
def applications(values):
    return words(len(values)) + b''.join(words(decl) + vector(args) for decl, args in values)
def constraints(values):
    return b''.join(words(markers) + applications(interfaces) for markers, interfaces in values)
def encode_body(model, root_upper, construction=False):
    functions, declarations = model['functions'], model['declarations']
    payload = bytearray(words(model['linkage'], len(functions), int(declarations is not None)))
    for function in functions:
        payload += counted(function['name']) + vector(function['parameters'])
        payload += words(function['result'], len(function['blocks']))
        for block in function['blocks']:
            assert len(block) == 4
            payload += words(*block)
        payload += words(len(function['instructions']))
        for instruction in function['instructions']:
            assert len(instruction) == 9
            payload += words(*instruction[:6]) + struct.pack('<q', instruction[6]) + words(*instruction[7:])
        payload += vector(function['operands'])
    if declarations is not None:
        assert len(declarations['identities']) == len(functions)
        payload += words(len(declarations['modules']), len(declarations['slots']), len(declarations['literals']),
                         declarations['root'], declarations['entry'])
        for name, dependencies, initializer in declarations['modules']:
            payload += counted(name) + vector(dependencies) + words(initializer)
        for identity in declarations['identities']:
            assert len(identity) == 9
            payload += words(*identity)
        for slot in declarations['slots']:
            assert len(slot) == 3
            payload += words(*slot)
        for literal in declarations['literals']:
            payload += counted(literal)
        payload += words(0)  # Both fixed models have no implementation records.
    generics = model['generics']
    payload += words(int(generics is not None))
    if generics is not None:
        assert len(generics) == len(functions)
        for kinds, conditions, arguments in generics:
            assert kinds is None or len(kinds) == len(conditions)
            payload += words(len(conditions), int(kinds is not None))
            if kinds is not None:
                payload += words(*kinds)
            payload += constraints(conditions) + vector(arguments)
    nodes, nominals, interfaces = model['nodes'], model['nominals'], model['interfaces']
    payload += words(len(nodes), len(nominals), len(interfaces))
    offsets = []
    for node in nodes:
        kind, span = node[:2]
        payload += words(kind, span)
        if kind == 1:
            parameters, result, _ = node[2:]
            payload += words(len(parameters))
            for parameter in parameters:
                assert len(parameter) == 2
                payload += words(*parameter)
            payload += words(result)
            offsets.append(64 + len(payload))
            payload += words(root_upper)
        elif kind in (2, 3, 5, 7, 8):
            payload += words(node[2])
        elif kind == 6:
            payload += vector(node[2])
        elif kind == 4:
            payload += words(node[2]) + vector(node[3]) + vector(node[4])
        else:
            raise AssertionError(('unknown kind in independent model', kind))
    for module, name, exported, kind, flags, native_id, fingerprint, conditions, fields, variants in nominals:
        fingerprint_bytes = bytes.fromhex(fingerprint)
        assert len(fingerprint_bytes) == 32
        payload += counted(module) + counted(name) + words(exported, kind, flags, native_id) + fingerprint_bytes
        payload += words(len(conditions)) + constraints(conditions) + words(len(fields))
        for field_name, type_id, field_flags in fields:
            payload += counted(field_name) + words(type_id, field_flags)
        payload += words(len(variants))
        for variant_name, begin, count in variants:
            payload += counted(variant_name) + words(begin, count)
    for module, name, exported, conditions, parents, methods in interfaces:
        payload += counted(module) + counted(name) + words(exported, len(conditions)) + constraints(conditions)
        payload += applications(parents) + words(len(methods))
        for method_name, signature, receiver, own_conditions in methods:
            payload += counted(method_name) + words(signature, receiver, len(own_conditions)) + constraints(own_conditions)
    if construction:
        assert not nominals
        payload += words(0)  # Mandatory dense construction owner: zero declarations.
    payload += words(len(model['defaults']))
    for default in model['defaults']:
        assert len(default) == 4
        payload += words(*default)
    payload += words(0)  # The definitions have no specialization provenance.
    return bytes(payload), offsets
def frame(model, semantic, upper=0, schema=26, construction=False):
    payload, offsets = encode_body(model, upper, construction)
    prefix = b'XRCHK\0\0\0' + words(schema, semantic, 2, 0) + struct.pack('<Q', len(payload))
    return prefix + hashlib.sha256(prefix + payload).digest() + payload, offsets
def literal(path, symbol):
    text = path.read_text('utf-8')
    match = re.search(r'\b' + re.escape(symbol) + r'\s*\[[^]]*\]\s*=\s*\{(.*?)\}', text, re.S)
    assert match, (path.name, symbol)
    body = re.sub(r'/\*.*?\*/|//[^\n]*', '', match.group(1), flags=re.S)
    return bytes(int(value, 0) for value in re.findall(r'0x[0-9a-fA-F]+|\b[0-9]+\b', body))
def array(symbol, data):
    rows = [', '.join('0x%02x' % byte for byte in data[i:i + 12]) for i in range(0, len(data), 12)]
    return 'static const uint8_t ' + symbol + '[] = {\n    ' + ',\n    '.join(rows) + '\n};\n'
def header(guard, rows, filename):
    text = ('/*\n * xray - Lightweight typed scripting with native concurrency\n'
            ' * https://www.xray-lang.org\n *\n'
            ' * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>\n'
            ' * Licensed under the MIT License\n *\n'
            ' * '+filename+' - Independent complete Library packet fields\n *\n'
            ' * KEY CONCEPT:\n'
            ' *   Complete fixed definition fields preserve all historical rejection vectors.\n'
            ' */\n#ifndef '+guard+'\n#define '+guard+'\n#include <stdint.h>\n'
            + ''.join(array(symbol, data) for symbol, data in rows) + '#endif // '+guard+'\n')
    assert len(text.splitlines())<=800
    return text

def output(path, text, write):
    if write:
        path.write_text(text, encoding='utf-8')
    else:
        assert path.read_text('utf-8') == text, ('current consumer vector mismatch', path.name)
def semantic_layout(model):
    at=64+12; result={}
    for f,function in enumerate(model['functions']):
        at+=4+len(function['name'].encode('utf-8'))+4+4*len(function['parameters'])+4
        at+=4+16*len(function['blocks'])+4
        for i,instruction in enumerate(function['instructions']):
            result[f'f{f}.i{i}.immediate']=at+24
            at+=40
        at+=4+4*len(function['operands'])
    d=model['declarations']
    if d is not None:
        result['literal_count']=at+8;at+=20
        for name,dependencies,initializer in d['modules']:
            at+=4+len(name.encode('utf-8'))+4+4*len(dependencies)+4
        at+=36*len(d['identities'])+12*len(d['slots'])
        for i,literal in enumerate(d['literals']):
            result[f'literal{i}.length']=at;result[f'literal{i}.bytes']=at+4
            at+=4+len(literal.encode('utf-8'))
    body,_=encode_body(model,8,True)
    result['default_count']=64+len(body)-8-16*len(model['defaults'])
    return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source-root',type=Path,required=True)
    parser.add_argument('--core-root',type=Path,required=True);parser.add_argument('--write',action='store_true');args=parser.parse_args()
    root=args.source_root;historical=root/'tests/unit/xir';core=args.core_root/'tests/unit/xir'
    model_bytes=(historical/'library2671_models.json').read_bytes()
    assert hashlib.sha256(model_bytes).hexdigest()==MODEL_SHA
    models=json.loads(model_bytes);records=[];headers={}
    checked=(args.core_root/'src/xir/xxir_checked.h').read_text('utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+27u',checked) and re.search(r'XR_XIR_CHECKED_CONTRACT\s+72u',checked)
    for oldsymbol,model in models.items():
        symbol=oldsymbol.replace('71','72')
        family='library_string' if symbol.startswith('library_string') else 'defaults'
        oldfile='xir_library_string71_goldens.h' if family=='library_string' else 'xir_defaults71_golden_bytes.h'
        old,old_flags=frame(model,71,8,26)
        assert old==literal(historical/oldfile,oldsymbol),(symbol,'full historical reproduction')
        current,flags=frame(model,72,8,27,True)
        assert not model['nominals'] and len(current)==len(old)+4 and flags==old_flags and current[-4:]==bytes(4)
        old_default=semantic_layout(model)['default_count']-4
        assert current[64:old_default]==old[64:old_default]
        assert current[old_default:old_default+4]==bytes(4)
        assert current[old_default+4:]==old[old_default:]
        if symbol=='defaults72_golden_8':assert current==literal(core/'xir_checked_scalar72_golden.h','checked_scalar72_golden')
        if symbol=='defaults72_golden_9':assert current==literal(core/'xir_generic_method72_golden.h','generic_method72_golden')
        headers.setdefault(family,[]).append((symbol,current))
        records.append({'symbol':symbol,'bytes':len(current),'sha256':hashlib.sha256(current).hexdigest(),'historical_sha256':hashlib.sha256(old).hexdigest(),'construction_count_offset':old_default,'semantic_offsets':semantic_layout(model)})
    for family,rows in headers.items():
        filename='xir_library_string72_goldens.h' if family=='library_string' else 'xir_defaults72_golden_bytes.h'
        text=header(filename.upper().replace('.','_'),rows,filename)
        if family=='library_string':
            alpha=semantic_layout(models['library_string71_alpha']);unused=semantic_layout(models['library_string71_unused'])
            constants={'ALPHA_LITERAL_ID_LOW':alpha['f0.i0.immediate'],'ALPHA_LITERAL_ID_HIGH':alpha['f0.i0.immediate']+4,
                'ALPHA_LITERAL_COUNT':alpha['literal_count'],'ALPHA_LITERAL0_LENGTH':alpha['literal0.length'],
                'ALPHA_LITERAL0_BYTES':alpha['literal0.bytes'],'UNUSED_LITERAL2_BYTES':unused['literal2.bytes']}
            assert list(constants.values())==[144,148,466,653,657,673]
            text=text.replace('#endif // '+filename.upper().replace('.','_')+'\n',''.join('#define LIBRARY_STRING72_'+key+' '+str(value)+'u\n' for key,value in constants.items())+'#endif // '+filename.upper().replace('.','_')+'\n')
        output(HERE/filename,text,args.write)
    print(json.dumps({'status':'INDEPENDENT_FULL_FIELDS_DERIVED','records':records,'current_writer_used':False,'build_tests':'NOT_RUN'},indent=2))
if __name__=='__main__':main()
