"""Exercise the fresh real reader on every complete current and historical input."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--reader',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    directory=Path(__file__).parent;args.output.mkdir(parents=True,exist_ok=True)
    projections=args.output/'semantic61';subprocess.run([sys.executable,'-B',str(directory/'derive_semantic61_migration.py'),'--output',str(projections)],check=True,timeout=30)
    history60=json.loads((directory/'semantic60_packet_history.json').read_text(encoding='utf8'))['packets']
    # Private codec goldens encode malformed function names; full readers reject them.
    codec_shape_rejections={
        ('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_embedded_golden'),
        ('tests/unit/xir/compile_owner/checked_sizing_cases.h','sizing_empty_golden')}
    assert len(history60)==23
    assert {(row['path'],row['name']) for row in history60}>=codec_shape_rejections
    current_statuses=[1 if (row['path'],row['name']) in codec_shape_rejections else 0 for row in history60]
    assert current_statuses.count(0)==21 and current_statuses.count(1)==2
    history59=json.loads((directory/'semantic59_packet_history.json').read_text(encoding='utf8'))['packets']
    history58=json.loads((directory/'test_roles_packet_vectors.json').read_text(encoding='utf8'))
    assert len(history59)==19 and len(history58)==18
    historical=args.output/'historical';historical.mkdir(exist_ok=True)
    for i,row in enumerate(history59):
        data=bytes.fromhex(row['packet_hex']);assert hashlib.sha256(data).hexdigest()==row['sha256'];(historical/f'{i:02d}-old59.chk').write_bytes(data)
    for i,row in enumerate(history58):
        data=bytes.fromhex(row['previous58_hex']);assert hashlib.sha256(data).hexdigest()==row['previous58_sha256'];(historical/f'{i:02d}-old58.chk').write_bytes(data)
    groups=[(sorted(projections.glob('*-current61.chk')),[1]*23,23),(sorted(projections.glob('*-old60.chk')),[1]*23,23),(sorted(historical.glob('*-old59.chk')),[1]*19,19),(sorted(historical.glob('*-old58.chk')),[1]*18,18)]
    records=[]
    for paths,statuses,count in groups:
        assert len(paths)==len(statuses)==count
        for path,expected in zip(paths,statuses):
            result=subprocess.run([str(args.reader.resolve()),str(path.resolve()),str(expected)],text=True,capture_output=True,timeout=30)
            row=dict(path=str(path.resolve()),expected=expected,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),exit=result.returncode,stdout=result.stdout,stderr=result.stderr)
            records.append(row);(args.output/'reader-results.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf8')
            assert result.returncode==0,row
    assert len(records)==83
    print('83 real reader probes PASS: historical61 23 rejected; historical positive and codec-shape roles preserved by the current62 reader gate / 23 old60 / 19 old59 / 18 old22/58')
if __name__=='__main__':main()
