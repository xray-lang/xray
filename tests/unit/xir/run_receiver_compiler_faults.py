"""Run all ten receiver compiler stages under one fresh-owner fault census and deadline."""
from __future__ import annotations
import time
STARTED=time.monotonic()
import argparse,ast,hashlib,json,os,re,subprocess,sys,tempfile,uuid
from pathlib import Path
import run_generic_callable_compiler_faults as owned

owned.SCHEMA='receiver-compiler-faults.v2'
WORKERS=8
TIMEOUT=600
CAPS=(67108864,8388608,128000000)
FIXTURE_PATH="tests/unit/xir/test_xir_generator_receiver_compiler.c"
FIXTURE_SYMBOL="receiver_compile_source"
FIXTURE_INPUTS=["tests/unit/xir/xir_generator_compiler_fault_cases.h"]

def snapshot(source,deadline):
    command=['git','-C',str(source),'rev-parse','--show-toplevel']
    probe=subprocess.run(command,capture_output=True,timeout=owned.remaining(deadline),check=False)
    if probe.returncode or Path(os.fsdecode(probe.stdout.strip())).resolve()!=source:
        return owned.snapshot(source,deadline)
    listing=subprocess.check_output(['git','-C',str(source),'ls-files','-z','--cached','--others','--exclude-standard'],
                                    timeout=owned.remaining(deadline))
    files={}
    for name in sorted(set(os.fsdecode(p)for p in listing.split(b'\0')if p)):
        path=source/name;owned.checked(path.is_file()and not path.is_symlink(),f'Invalid actual Source member: {name}')
        files[name]=owned.sha(path,deadline)
    owned.checked(files,'Empty actual Source inventory')
    return files

def fixture_identity(source):
    text=(source/FIXTURE_PATH).read_text(encoding='utf-8')
    match=re.search(rf'static const char {re.escape(FIXTURE_SYMBOL)}\[\]\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);',text,re.S)
    owned.checked(match,'Missing actual compiled receiver Source literal')
    data=''.join(ast.literal_eval(s)for s in re.findall(r'"(?:[^"\\]|\\.)*"',match[1])).encode('ascii')
    return len(data),hashlib.sha256(data).hexdigest()

def census(row):
    owned.checked(row.get('event')=='census'and row.get('public_calls')==10 and row.get('ledger_OOM')==1,
                  'Ten complete stages or the ledger constructor fault are missing')
    sites=owned.integer(row,'sites',1);owned.checked(sites<=65000,'Original allocation census cap exceeded')
    required=[owned.integer(row,k,1)for k in ('allocated','peak','work')]
    owned.checked(all(n<=cap for n,cap in zip(required,CAPS)),'Original whole-operation caps exceeded')
    owned.checked(owned.integer(row,'live')==owned.integer(row,'owner_live',1),'Census ledger did not return to baseline')
    owned.physical_zero(row)
    return sites,required

def process_report(path,identity,mode,index,sites,points,deadline):
    rows=owned.records(path,deadline)
    expected={'event':'identity','schema':owned.SCHEMA,'mode':mode,'index':index,'sites':sites,'workers':8,
              **identity,'allocated_cap':CAPS[0],'live_cap':CAPS[1],'work_cap':CAPS[2]}
    owned.checked(rows and rows[0]==expected,'Child executable, source, configuration or partition identity differs')
    owned.checked(len(rows)>=2,'Missing complete child report');complete=rows[-1]
    histories=owned.integer(complete,'histories');want=len(rows)-2 if mode in {'shard','probe'}else 0
    owned.checked(histories==want and complete=={'event':'complete','points':points,'histories':histories,
                  'compiler_blocks':0,'compiler_bytes':0},'Missing exact primary/history completion')
    return rows[1:-1]

def verify_histories(rows,index,sites,roots,deadline):
    at=0;histories=0;suffixes=0
    for ordinal in range(index,sites,8):
        owned.checked(ordinal not in roots,'Duplicate original primary ordinal');roots.add(ordinal)
        stack=[(ordinal,)]
        while stack:
            owned.remaining(deadline);history=stack.pop()
            owned.checked(at<len(rows),'Missing actual allocation failure history');row=rows[at];at+=1
            owned.checked(row.get('event')=='point'and row.get('ordinal')==ordinal and row.get('history')==list(history)
                          and row.get('fired')==len(history),'Wrong, duplicate or unrooted failure history')
            attempts=owned.integer(row,'attempts',1)
            owned.checked(history[-1]<attempts<=65000 and row.get('status')==7 and row.get('injected')is True,
                          'Failure history did not reach every requested physical OOM')
            owned.checked(owned.integer(row,'live')==owned.integer(row,'owner_live',1),'History leaked its fresh owner')
            for key,cap in zip(('allocated','peak','work'),CAPS):
                owned.checked(owned.integer(row,key)<=cap,'History exceeded an original whole-operation cap')
            owned.integer(row,'allocations');owned.physical_zero(row);histories+=1;suffixes+=len(history)>1
            for next_ordinal in reversed(range(history[-1]+1,attempts)):stack.append(history+(next_ordinal,))
    owned.checked(at==len(rows),'Foreign or extra allocation failure histories')
    return histories,suffixes

def main():
    deadline=STARTED+TIMEOUT
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('executable','source-root','build-root'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--reports-root',type=Path,default=Path(tempfile.gettempdir())/'xray-receiver-compiler')
    args=parser.parse_args();owned.checked(os.name=='nt','This runner requires the owned Windows Job adapter')
    source=args.source_root.resolve(strict=True);build=args.build_root.resolve(strict=True)
    reports=args.reports_root.resolve();owned.checked(not reports.is_relative_to(source)and not reports.is_relative_to(build),'Reports overlap an input')
    out=reports/(time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex);out.mkdir(parents=True,exist_ok=False)
    active=[];owner=None
    manifest={'qualified':False,'schema':owned.SCHEMA,'workers':WORKERS,'global_timeout':TIMEOUT,'commands':[],
              'scope':'Complete ten-stage compact receiver Source, every original normal allocation ordinal and six exact/minus-one axes',
              'full_P08':False,'static_consumer_ROOT_qualification':False}
    def publish():
        with(out/'qualification.json').open('w',encoding='utf-8',newline='\n')as stream:
            json.dump(manifest,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    def launch(name,arguments):
        owned.remaining(deadline);command=[str(executable),*arguments,nonce,str(out/(name+'.jsonl'))]
        info=subprocess.STARTUPINFO();info.dwFlags|=subprocess.STARTF_USESHOWWINDOW;info.wShowWindow=0
        with(out/(name+'.stdout.log')).open('xb')as stdout,(out/(name+'.stderr.log')).open('xb')as stderr:
            child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=stdout,stderr=stderr,env=environment,
                close_fds=True,startupinfo=info,creationflags=subprocess.CREATE_NO_WINDOW)
        row={'name':name,'argv':command,'pid':child.pid,'started_seconds':time.monotonic()-STARTED}
        active.append((child,row));manifest['commands'].append(row);publish()
    def drain():
        while active:
            owned.remaining(deadline)
            for child,row in list(active):
                status=child.poll()
                if status is not None:
                    active.remove((child,row));row.update(exit_code=status,ended_seconds=time.monotonic()-STARTED);publish()
                    owned.checked(status==0,f'{row["name"]} exited {status}; inspect {out}')
            if active:time.sleep(min(.01,owned.remaining(deadline)))
    try:
        owner={'adapter':'windows-job','job':owned.establish_job()};executable=args.executable.resolve(strict=True)
        inputs=snapshot(source,deadline);configuration=owned.config_snapshot(build,deadline)
        names=['tests/unit/xir/test_xir_generator_receiver_compiler.c','tests/unit/xir/run_receiver_compiler_faults.py',
               'tests/unit/xir/run_generic_callable_compiler_faults.py','tests/unit/xir/generator_driver.cmake',
               'tests/unit/xir/xir_library_compile_owner.h','tests/unit/xir/xir_source_program_compile_owner.h',
               'tests/lib/xraytest/windows_owned_job.py','tests/lib/xraytest/proc.py',*FIXTURE_INPUTS]
        owned.checked(all(name in inputs for name in names),'Actual driver, registry or finite owner input is missing')
        length,digest=fixture_identity(source);binary=owned.sha(executable,deadline);nonce=uuid.uuid4().hex
        identity={'nonce':nonce,'input_sha256':owned.mapping_sha(inputs),'binary_sha256':binary,
                  'config_sha256':owned.mapping_sha(configuration),'source_bytes':length,'source_sha256':digest}
        environment=os.environ.copy();environment['XRAY_RECEIVER_FI_STDLIB']=str(source/'stdlib')
        for key in ('input','binary','config'):environment[f'XRAY_GENERIC_SHARD_{key.upper()}_SHA256']=identity[f'{key}_sha256']
        manifest.update(executable=str(executable),source_root=str(source),build_root=str(build),inputs=inputs,
                        configuration=configuration,identity=identity,native_owner=owner);publish()
        launch('census',['--compiler-census']);drain()
        rows=process_report(out/'census.jsonl',identity,'census',0,0,0,deadline)
        owned.checked(len(rows)==1,'Census contains fault trials or misses the denominator')
        sites,required=census(rows[0]);manifest.update(census=rows[0],required_caps=required);publish()
        for index in range(WORKERS):launch(f'shard-{index}',['--compiler-shard',str(index),str(sites)])
        drain();union=set();histories=0;suffixes=0
        for index in range(WORKERS):
            rows=process_report(out/f'shard-{index}.jsonl',identity,'shard',index,sites,len(range(index,sites,WORKERS)),deadline)
            count,extra=verify_histories(rows,index,sites,union,deadline);histories+=count;suffixes+=extra
        owned.checked(union==set(range(sites)),'Missing original allocation ordinal')
        manifest.update(completed_ordinals=sorted(union),complete_fault_count=len(union),
                        complete_histories=histories,allocating_failure_suffix_histories=suffixes);publish()
        launch('bounds',['--compiler-bounds',str(sites),*(str(n)for n in required)]);drain()
        rows=process_report(out/'bounds.jsonl',identity,'bounds',0,sites,6,deadline);owned.verify_bounds(rows,required)
        manifest['boundaries']=rows
        owned.checked(snapshot(source,deadline)==inputs and owned.config_snapshot(build,deadline)==configuration and
                      owned.sha(executable,deadline)==binary,'Actual input, configuration or binary changed')
        manifest['native_drain']=owned.owned_drain(owner,deadline)
        names=['census',*(f'shard-{i}'for i in range(WORKERS)),'bounds']
        outputs={name+suffix for name in names for suffix in ('.jsonl','.stdout.log','.stderr.log')}
        owned.checked({p.name for p in out.iterdir()}==outputs|{'qualification.json'},'Undeclared report output')
        manifest['outputs']={name:owned.sha(out/name,deadline)for name in sorted(outputs)}
        manifest.update(qualified=True,failure_tree_qualified=True,seconds=time.monotonic()-STARTED);publish();owned.remaining(deadline)
        print(f'Receiver compiler: {sites} primary faults, {histories} complete histories including {suffixes} suffix histories, ten stages and six axes; {out}')
        return 0
    except Exception as error:
        manifest.update(qualified=False,failure=str(error),seconds=time.monotonic()-STARTED);publish()
        print(f'Receiver compiler FAIL: {error}; {out}',file=sys.stderr);return 1
    finally:
        for child,_ in active:
            if child.poll()is None:child.kill()
        for child,row in active:
            try:row['exit_code']=child.wait(timeout=max(.01,deadline-time.monotonic()))
            except subprocess.TimeoutExpired:row['exit_pending_parent_job_close']=True
            row['terminated_on_failure']=True
        if active:publish()

if __name__=='__main__':raise SystemExit(main())
