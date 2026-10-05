from pathlib import Path
import hashlib,json,struct,re
from derive_test_roles_vectors import upgrade, historical_vector
from derive_semantic61_migration import semantic61_packet
from derive_tuple62_migration import tuple62_packet
from derive_tuple63_migration import tuple63_packet
from derive_assert_atomic64_packet import current64_packet

def words(*values): return struct.pack('<'+'I'*len(values),*values)
def op(code,result=0,left=0,right=0,immediate=0):
    return words(code,result,left,right,0,0)+struct.pack('<q',immediate)+words(0,0)
def function(name,parameters,result,instructions):
    name=name.encode('ascii')
    return words(len(name))+name+words(len(parameters))+words(*parameters)+words(result,1,0,len(instructions),0,0,len(instructions))+b''.join(instructions)+words(0)
def packet(body,contract,schema=22):
    head=b'XRCHK\0\0\0'+words(schema,contract,2,0)+struct.pack('<Q',len(body))
    return head+hashlib.sha256(head+body).digest()+body
# Fixed enum identities are independently specified by the condition contract.
# Unit0/Bool1/String3, RETURN25/CONST_STRING3/ASSERT_CONDITION115.
identity=b'memory-module-v1:id=23:xray-core-assertions-v1'
body=words(1,3,1)
body+=function('$init',[],0,[op(25)])
body+=function('assert',[1,3],0,[op(115,left=0,right=1),op(25)])
body+=function('$argument_default',[],3,[op(3,3),op(25,left=0)])
body+=words(1,0,1,0xffffffff,0xffffffff)
body+=words(len(identity))+identity+words(0,0)
for exported in (0,1,0): body+=words(0,exported,0,0,0,0,0)
body+=words(0,0) # Empty literal0, implementation count0.
body+=words(0,0,0,0) # No generics or type/nominal/interface pools.
body+=words(1,0,1,1,2) # One default: parameter owner1 ordinal1 helper2.
body+=words(0) # No provenance on this unspecialized library.
current=tuple63_packet(tuple62_packet(semantic61_packet(upgrade(packet(body,58)))))
directory=Path(__file__).parent
manifest=json.loads((directory/'assert_condition_packet_vectors.json').read_text(encoding='utf-8'))
header=(directory/'xir_assert_condition_golden.h').read_text(encoding='utf-8')
golden=bytes(int(value,16) for value in re.findall(r'0x[0-9a-fA-F]{2}',header))
assert current==golden and packet(body,57).hex()==manifest['core_packet_hex']
current64=current64_packet(current)
header64=(directory/'xir_assert_condition64_golden.h').read_text(encoding='utf-8')
assert bytes(int(value,16) for value in re.findall(r'0x[0-9a-fA-F]{2}',header64))==current64
assert hashlib.sha256(body).hexdigest()==manifest['core_body_sha256']
assert packet(body,55,21).hex()==manifest['previous55_core_packet_hex']
for record in manifest['same_body_vectors']:
    text=(directory/record['path']).read_text(encoding='utf-8')
    match=re.search(r'static const uint8_t '+record['name']+r'\[\] = \{(.*?)\};',text,re.S)
    literal=historical_vector(record['path'],record['name'])
    assert packet(literal[64:],58)==literal
    assert hashlib.sha256(literal[64:]).hexdigest()==record['body_sha256']
    assert packet(literal[64:],54,21)[32:64].hex()==record['old_digest']
    assert packet(literal[64:],55,21)[32:64].hex()==record['previous55_digest']
    assert packet(literal[64:],57)[32:64].hex()==record['current_digest']
print(json.dumps({'core_bytes':len(current64),'core_digest':current64[32:64].hex(),
    'complete_previous24_63_sha256':hashlib.sha256(current).hexdigest(),
    'previous54_whole_bodies_reproduced':len(manifest['same_body_vectors']),
    'historical_wire22_semantic58_whole_bodies_verified':len(manifest['same_body_vectors'])},indent=2))
