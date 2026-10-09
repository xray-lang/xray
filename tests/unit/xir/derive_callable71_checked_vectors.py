"""Encode complete scalar and generic-method consumer packets independently.

The models contain all definition fields. Historical literals are comparison
inputs only; no compiler writer or existing generator is imported or invoked.
"""
import argparse, copy, hashlib, json, re, struct, sys
from pathlib import Path
sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MODEL_SHA = 'c32b9c85a8194ababd69079b9ca5ced8d44c7295ec4120b56ccebacba4b39550'
MODELS = {'8': {'linkage': 0,
       'functions': [{'name': 'n',
                      'parameters': [],
                      'result': 2,
                      'blocks': [[0, 2, 0, 0]],
                      'instructions': [[2, 2, 0, 0, 0, 0, -9223372036854775808, 0, 0],
                                       [33, 0, 0, 0, 0, 0, 0, 0, 0]],
                      'operands': []}],
       'declarations': None,
       'generics': None,
       'nodes': [],
       'nominals': [],
       'interfaces': [],
       'defaults': []},
 '14': {'linkage': 0,
        'functions': [{'name': 'main',
                       'parameters': [],
                       'result': 2,
                       'blocks': [[0, 2, 0, 0]],
                       'instructions': [[2, 2, 0, 0, 0, 0, 41, 0, 0], [33, 0, 0, 0, 0, 0, 0, 0, 0]],
                       'operands': []},
                      {'name': 'init',
                       'parameters': [],
                       'result': 0,
                       'blocks': [[0, 1, 0, 0]],
                       'instructions': [[33, 0, 0, 0, 0, 0, 0, 0, 0]],
                       'operands': []}],
        'declarations': {'modules': [['alpha', [], 1]],
                         'identities': [[0, 1, 0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0, 0, 0, 0]],
                         'slots': [],
                         'literals': [],
                         'root': 0,
                         'entry': 0},
        'generics': None,
        'nodes': [[1, 2, [[65537, 0]], 65537, 0]],
        'nominals': [],
        'interfaces': [['alpha', 'Evidence', 1, [[0, []]], [], []],
                       ['alpha',
                        'Parent',
                        1,
                        [[0, []]],
                        [],
                        [['map', 256, 0, [[1, [[0, [65536]]]]]],
                         ['copy', 256, 0, [[0, [[0, [65536]]]]]]]]],
        'defaults': []}}

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
def encode_body(model, root_upper):
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
    payload += words(len(model['defaults']))
    for default in model['defaults']:
        assert len(default) == 4
        payload += words(*default)
    payload += words(0)  # The definitions have no specialization provenance.
    return bytes(payload), offsets
def frame(model, semantic, upper=0, schema=26):
    payload, offsets = encode_body(model, upper)
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
def header(guard, rows):
    return ('/* Independently encoded full definition fields; historical packets remain separate. */\n'
            '#ifndef ' + guard + '\n#define ' + guard + '\n#include <stdint.h>\n'
            + ''.join(array(symbol, data) for symbol, data in rows) + '#endif\n')
def output(path, text, write):
    if write:
        path.write_text(text, encoding='utf-8')
    else:
        assert path.read_text('utf-8') == text, ('current consumer vector mismatch', path.name)
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,default=ROOT)
    parser.add_argument('--write',action='store_true')
    args=parser.parse_args(); root=args.source_root; historical=root/'tests/unit/xir'
    checked=(root/'src/xir/xxir_checked.h').read_text('utf-8')
    assert re.search(r'XR_XIR_CHECKED_SCHEMA\s+26u',checked) and re.search(r'XR_XIR_CHECKED_CONTRACT\s+71u',checked)
    public=(root/'src/xir/xxir.h').read_text('utf-8')
    assert re.search(r'XR_XIR_CALLABLE_ROOT_UNRESOLVED\s+8u',public)
    operations=re.findall(r'^XR_XIR_OP\((\w+)',(root/'src/xir/xxir_ops.def').read_text('utf-8'),re.M)
    assert len(operations)==151 and operations[1]=='CONST_INT' and operations[32]=='RETURN'
    scalar,_=frame(MODELS['8'],71,8)
    generic,offsets=frame(MODELS['14'],71,8)
    invalid0,_=frame(MODELS['14'],71,0);invalid1,_=frame(MODELS['14'],71,1)
    assert len(scalar)==221 and len(generic)==616 and offsets==[437]
    map_own=generic.index(b'map')+3+8
    copy_own=generic.index(b'copy')+4+8
    assert (map_own,copy_own)==(544,584)
    # The changed schema extends non-ABSENT provenance. These fully encoded
    # definition models end in ABSENT and retain every body field and offset.
    histories=[]
    for filename,symbol,model,upper,current in [
        ('xir_checked_scalar70_golden.h','checked_scalar70_golden',MODELS['8'],8,scalar),
        ('xir_generic_method70_golden.h','generic_method70_golden',MODELS['14'],8,generic),
        ('xir_generic_method70_golden.h','generic_method70_invalid_flags0',MODELS['14'],0,invalid0),
        ('xir_generic_method70_golden.h','generic_method70_invalid_flags1',MODELS['14'],1,invalid1)]:
        previous,_=frame(model,70,upper,schema=25)
        assert previous==literal(historical/filename,symbol)
        assert previous[64:]==current[64:] and previous[-4:]==b'\0'*4
        histories.append({'symbol':symbol,'bytes':len(previous),'sha256':hashlib.sha256(previous).hexdigest()})
    output(HERE/'xir_checked_scalar71_golden.h',header('XIR_CHECKED_SCALAR71_GOLDEN_H',[
        ('checked_scalar71_golden',scalar),('checked_scalar71_digest',scalar[32:64])]),args.write)
    generic_header=header('XIR_GENERIC_METHOD71_GOLDEN_H',[
        ('generic_method71_golden',generic),('generic_method71_invalid_flags0',invalid0),
        ('generic_method71_invalid_flags1',invalid1)])
    generic_header=generic_header.replace('#endif\n','#define GENERIC_METHOD71_MAP_OWN 544u\n#define GENERIC_METHOD71_COPY_OWN 584u\n#endif\n')
    output(HERE/'xir_generic_method71_golden.h',generic_header,args.write)
    print(json.dumps({'status':'INDEPENDENT_FULL_FIELDS_26_71_DERIVED','schema':26,'semantic':71,
        'scalar_bytes':len(scalar),'scalar_digest':scalar[32:64].hex(),
        'generic_bytes':len(generic),'generic_flags_offset':offsets[0],
        'generic_map_own':map_own,'generic_copy_own':copy_own,'old70_complete_reproduced':histories,
        'writer_outputs_used':False,'Xray_build_and_runtime':'NOT_RUN'},indent=2))
if __name__=='__main__':main()
