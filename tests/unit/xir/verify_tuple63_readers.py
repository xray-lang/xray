"""Run every current complete packet and preserved historical revision against one reader."""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path
from derive_semantic61_migration import semantic61_packet
from derive_tuple63_migration import tuple63_packet
from derive_tuple63_vectors import tuple_packet
from derive_tuple65_consumers import current_family,verify_literals

def valid(data):
    assert len(data)>=64 and data[:8]==b'XRCHK\0\0\0'
    assert struct.unpack_from('<II',data,16)==(2,0)
    assert struct.unpack_from('<Q',data,24)[0]==len(data)-64
    assert hashlib.sha256(data[:32]+data[64:]).digest()==data[32:64]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--reader',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--historical62',action='store_true');args=parser.parse_args()
    directory=Path(__file__).parent;args.output.mkdir(parents=True,exist_ok=True)
    rows62=json.loads((directory/'tuple62_packet_history.json').read_text(encoding='utf-8'))['packets']
    rows60=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf-8'))['packets']
    rows59=json.loads((directory/'semantic59_packet_history.json').read_text(encoding='utf-8'))['packets']
    rows58=json.loads((directory/'test_roles_packet_vectors.json').read_text(encoding='utf-8'))
    rejects={('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_embedded_golden'),
             ('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_empty_golden')}
    assert (len(rows62),len(rows60),len(rows59),len(rows58))==(25,23,19,18)
    verify_literals();probes=[];positive=0
    for i,row in enumerate(rows62):
        old62=bytes.fromhex(row['old62_hex']);valid(old62)
        assert struct.unpack_from('<II',old62,8)==(24,62)
        assert hashlib.sha256(old62).hexdigest()==row['old62_sha256']
        assert len(old62)==row['length'] and hashlib.sha256(old62[64:]).hexdigest()==row['body_sha256']
        expected=int((row['path'],row['name']) in rejects);positive+=expected==0
        if args.historical62:probes.append((f'{i:02d}-historical62',old62,expected));continue
        current=tuple63_packet(old62);valid(current)
        assert hashlib.sha256(current).hexdigest()==row['current63_sha256'] and current[64:]==old62[64:]
        if row['name']=='tuple_24_62':assert current==tuple_packet()
        previous64=current_family(row,current,64);current65=current_family(row,current,65)
        valid(previous64);valid(current65);assert current65[64:]==previous64[64:]
        probes.extend([(f'{i:02d}-current65',current65,expected),(f'{i:02d}-old64',previous64,1),
                       (f'{i:02d}-old63',current,1),(f'{i:02d}-old62',old62,1)])
    assert positive==23
    if not args.historical62:
        for i,row in enumerate(rows60):
            old60=bytes.fromhex(row['old60_hex']);old61=semantic61_packet(old60)
            assert hashlib.sha256(old60).hexdigest()==row['old60_sha256']
            assert hashlib.sha256(old61).hexdigest()==row['current61_sha256']
            assert hashlib.sha256(old60[64:]).hexdigest()==row['body_sha256']
            probes.extend([(f'{i:02d}-old61',old61,1),(f'{i:02d}-old60',old60,1)])
        for i,row in enumerate(rows59):
            data=bytes.fromhex(row['packet_hex']);assert hashlib.sha256(data).hexdigest()==row['sha256'];probes.append((f'{i:02d}-old59',data,1))
        for i,row in enumerate(rows58):
            data=bytes.fromhex(row['previous58_hex']);assert hashlib.sha256(data).hexdigest()==row['previous58_sha256'];probes.append((f'{i:02d}-old58',data,1))
    assert len(probes)==(25 if args.historical62 else 183);records=[]
    for name,data,expected in probes:
        valid(data);path=args.output/(name+'.chk');path.write_bytes(data)
        completed=subprocess.run([str(args.reader.resolve()),str(path.resolve()),str(expected)],text=True,capture_output=True,timeout=30)
        record={'name':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                'schema':struct.unpack_from('<I',data,8)[0],'semantic':struct.unpack_from('<I',data,12)[0],
                'body_sha256':hashlib.sha256(data[64:]).hexdigest(),'expected':expected,
                'exit':completed.returncode,'stdout':completed.stdout,'stderr':completed.stderr}
        records.append(record);(args.output/'results.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        assert completed.returncode==0,record
    print(('25 historical62 real-reader probes: 23 positives + 2 name-shape rejects' if args.historical62 else
           '183 current65 real-reader probes: 23 positives + 2 name-shape rejects, 25 old64, 25 old63, 25 old62, 23 old61, 23 old60, 19 old59, 18 old58')+' PASS')

if __name__=='__main__':main()
