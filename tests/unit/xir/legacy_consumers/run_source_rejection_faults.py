"""Verify every real SDK compiler allocation cut and exact resource boundaries."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse,hashlib,json,os,re,tempfile,time
import task_rejection_model

ROOT = Path(__file__).resolve().parent.parents[3]
digest = lambda data: hashlib.sha256(data).hexdigest()


def review_records(sites, jobs, shards):
    if type(sites) is not int or sites <= 0 or type(jobs) is not int or not 1 <= jobs <= 8:
        raise ValueError('SDK fault review requires positive measured sites and one to eight workers')
    all_ordinals = []
    reviews = []
    pattern = re.compile(r'sdk-fault ordinal=(\d+) code=(\d+) oom=(\d+) injected=(\d+) attempts=(\d+) physical=0/0')
    for index, row in enumerate(shards):
        lines = row['stdout'].decode('utf-8').splitlines()
        values = [pattern.fullmatch(line) for line in lines if line.startswith('sdk-fault ')]
        if any(value is None for value in values): raise ValueError('Malformed SDK fault record')
        ordinals = [int(value[1]) for value in values]
        expected = list(range(index, sites, jobs))
        summary = re.search(r'sdk-summary sites=(\d+) shard=(\d+) shards=(\d+) covered=(\d+) physical=0/0',row['stdout'].decode())
        ok = (row['exit_code'] == 0 and not row['timed_out'] and not row['stderr'] and ordinals == expected
              and all(int(value[2]) != 0 and int(value[3]) == int(value[4]) == 1
                      and int(value[5]) > int(value[1]) for value in values)
              and summary is not None and list(map(int,summary.groups())) == [sites,index,jobs,len(expected)])
        if not ok: raise ValueError('Incomplete or unqualified SDK compiler fault shard: '+str(index))
        all_ordinals.extend(ordinals); reviews.append({'shard':index,'covered':len(ordinals),'complete':True})
    if len(shards) != jobs or len(all_ordinals) != sites or sorted(all_ordinals) != list(range(sites)):
        raise ValueError('SDK compiler cuts are missing, duplicated or outside the measured range')
    return reviews


def main():
    import run_source_rejection_driver
    from xraytest import proc
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--owner-manifest',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--jobs',type=int,choices=range(1,9),default=8)
    parser.add_argument('--measure',action='store_true',help='Only development baseline/axes; grants no complete fault qualification')
    args = parser.parse_args(); started = time.monotonic(); deadline = started+120
    parent = args.output.resolve(); parent.mkdir(parents=True,exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='faults-',dir=parent))
    binary = args.binary.resolve(strict=True); manifest_path = args.owner_manifest.resolve(strict=True)
    manifest_bytes = manifest_path.read_bytes(); manifest = json.loads(manifest_bytes)
    binding = run_source_rejection_driver.checked_owner(binary,Path(manifest['owner_binary']),manifest_path)
    fault = Path(binding['fault_owner_binary']); oracle = task_rejection_model.expected()
    programs = [oracle['source'], *(row['source'] for row in oracle['controls'])]
    physical = []
    for index,name in enumerate(programs):
        raw = (ROOT/name).read_bytes()
        if digest(raw.replace(b'\r\n',b'\n')) != task_rejection_model._INPUTS[name]:
            raise ValueError('SDK fault source changed')
        path=output/('input'+str(index)+'.xr');path.write_bytes(raw);physical.append(path)
    environment=dict(os.environ,XRAY_STDLIB_PATH=str(ROOT/'stdlib'),NO_COLOR='1',PYTHONUTF8='1')
    def invoke(label, extra):
        command=[str(fault),*map(str,physical),str(ROOT/'stdlib'),*extra]
        observed=proc.run(command,cwd=output,env=environment,timeout=max(.001,deadline-time.monotonic()))
        stdout,stderr,timed_out=observed.stdout,observed.stderr,observed.timed_out
        (output/(label+'.stdout')).write_bytes(stdout);(output/(label+'.stderr')).write_bytes(stderr)
        return {'label':label,'argv':command,'exit_code':observed.returncode,'timed_out':timed_out,'stdout':stdout,'stderr':stderr}
    baseline=invoke('baseline',['--baseline'])
    match=re.search(rb'sdk-baseline sites=(\d+) allocated=(\d+) peak=(\d+) work=(\d+) physical=0/0',baseline['stdout'])
    if baseline['exit_code'] or baseline['timed_out'] or baseline['stderr'] or not match:
        raise ValueError('SDK compiler fault baseline failed')
    sites=int(match[1]);axes=invoke('axes',['--axes'])
    axis_lines=[line for line in axes['stdout'].decode().splitlines() if line.startswith('sdk-axis ')]
    expected_axes=['sdk-axis axis='+str(axis)+' below='+str(below)+' limit='+str(int(match[axis+2])-below)
                   +' code='+str(below)+' budget='+str(below)+' injected=0 physical=0/0'
                   for axis in range(3) for below in range(2)]
    if axes['exit_code'] or axes['timed_out'] or axes['stderr'] or axis_lines != expected_axes:
        raise ValueError('SDK resource exact/one-below boundaries failed')
    commands=[baseline,axes];reviews=[]
    if not args.measure:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            shards=list(pool.map(lambda index:invoke('shard-'+str(index),['--shard',str(index),str(args.jobs)]),range(args.jobs)))
        commands+=shards;reviews=review_records(sites,args.jobs,shards)
    if (manifest_bytes != manifest_path.read_bytes() or task_rejection_model.expected() != oracle
            or run_source_rejection_driver.checked_owner(binary,Path(binding['owner_binary']),manifest_path) != binding):
        raise ValueError('SDK fault inputs changed during execution')
    seconds=time.monotonic()-started
    if seconds>120:raise ValueError('SDK fault qualification exceeded the original120s total budget')
    report={'scope':'BASELINE_AXES_ONLY_NO_FULL_FI' if args.measure else 'COMPLETE_SDK_COMPILER_ALLOCATOR_CUTS',
        'sites':sites,'covered':sum(row['covered'] for row in reviews),'shards':reviews,'resource_axes':axis_lines,
        'commands':[{key:value for key,value in row.items() if key not in ['stdout','stderr']}
                    | {'stdout_sha256':digest(row['stdout']),'stderr_sha256':digest(row['stderr'])} for row in commands],
        'binding_manifest_sha256':digest(manifest_bytes),'seconds':seconds,'timeout_seconds':120,
        'full_compiler_FI_qualified':not args.measure,'physical_compiler_domain_zero':True,
        'native_runtime_FI_or_sanitizer_qualified':False,'production_release':False,'whole_goal_complete':False}
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:report[key] for key in ['scope','sites','covered','seconds','full_compiler_FI_qualified']}))
    return 0


if __name__=='__main__':raise SystemExit(main())
