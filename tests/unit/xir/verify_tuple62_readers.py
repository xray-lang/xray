"""Keep every previous current positive and complete historical input observable."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from derive_semantic61_migration import semantic61_packet
from derive_tuple62_migration import tuple62_packet
from derive_tuple62_vectors import tuple_packet

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--reader',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    directory=Path(__file__).parent;args.output.mkdir(parents=True,exist_ok=True)
    rows60=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf-8'))['packets']
    rows59=json.loads((directory/'semantic59_packet_history.json').read_text(encoding='utf-8'))['packets']
    rows58=json.loads((directory/'test_roles_packet_vectors.json').read_text(encoding='utf-8'))
    rejects={('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_embedded_golden'),
             ('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_empty_golden')}
    assert len(rows60)==23 and len(rows59)==19 and len(rows58)==18
    probes=[];positive=0
    for i,row in enumerate(rows60):
        old60=bytes.fromhex(row['old60_hex']);old61=semantic61_packet(old60)
        assert hashlib.sha256(old60).hexdigest()==row['old60_sha256']
        assert hashlib.sha256(old61).hexdigest()==row['current61_sha256']
        assert len(old60)==row['length'] and hashlib.sha256(old60[64:]).hexdigest()==row['body_sha256']
        current=tuple62_packet(old61);expected=int((row['path'],row['name']) in rejects);positive+=expected==0
        probes.extend([(f'{i:02d}-current62',current,expected),(f'{i:02d}-old61',old61,1),(f'{i:02d}-old60',old60,1)])
    assert positive==21
    for i,row in enumerate(rows59):
        data=bytes.fromhex(row['packet_hex']);assert hashlib.sha256(data).hexdigest()==row['sha256'];probes.append((f'{i:02d}-old59',data,1))
    for i,row in enumerate(rows58):
        data=bytes.fromhex(row['previous58_hex']);assert hashlib.sha256(data).hexdigest()==row['previous58_sha256'];probes.append((f'{i:02d}-old58',data,1))
    assert len(probes)==106;probes.append(('tuple62',tuple_packet(),0));records=[]
    for name,data,expected in probes:
        path=args.output/(name+'.chk');path.write_bytes(data)
        completed=subprocess.run([str(args.reader.resolve()),str(path.resolve()),str(expected)],text=True,capture_output=True,timeout=30)
        row={'name':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'expected':expected,'exit':completed.returncode,'stdout':completed.stdout,'stderr':completed.stderr}
        records.append(row);(args.output/'results.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
        assert completed.returncode==0,row
    print('107 real reader probes PASS: current62 original 21 positives + 2 name-shape rejects, 23 old61, 23 old60, 19 old59, 18 old58, independent Tuple KAT')
if __name__=='__main__':main()
