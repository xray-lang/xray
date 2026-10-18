"""Real CLI handoff death and Windows API failures, observed outside the application."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
from stdio_win_debug import WinDebugger,U32,PTR
from test_stdio_client import ClientBase,wire

parser=argparse.ArgumentParser()
parser.add_argument('--xray',type=Path,required=True)
parser.add_argument('--paused-fixture',type=Path,required=True)
parser.add_argument('--streams-fixture',type=Path,required=True)
parser.add_argument('--case',choices=('postcommit_death','query_reject','warm'),default='postcommit_death')
parser.add_argument('--resources',action='store_true')
parser.add_argument('--transcript',type=Path,required=True)
args=parser.parse_args()
config=SimpleNamespace(xray=args.xray,transport='stdio')
PRE='前 out\0αtrue\ntrue\n'.encode();ERR_PRE='前 err\0α'.encode()
ALL=PRE+'后 out\0βtrue\ntrue\n'.encode();ERR_ALL=ERR_PRE+'后 err\0β'.encode()
MODES=('query_reject','set_reject','parent_reset','parent_wait','ack_fail','close_process','close_job',
       'flush_stdout','flush_stderr','reset_commit','reset_reject','signal_request','wait_decision','query_after_parent_death','reject_ack')

def exercise(debugger):
    client=ClientBase(config,process_factory=lambda *unused,**options:debugger.process)
    target=None;k=debugger.kernel
    k.OpenProcess.argtypes=(U32,C.c_int,U32);k.OpenProcess.restype=PTR
    k.WaitForSingleObject.argtypes=(PTR,U32);k.GetExitCodeProcess.argtypes=(PTR,C.POINTER(U32))
    try:
        initialized=client.send('initialize');assert initialized['success'],initialized
        retained=initialized['body']['xrayRetainedOutput'];pid=retained['processId']
        assert pid==debugger.worker_pid and pid!=debugger.root_pid
        paths=[Path(retained[key]) for key in ('stdout','stderr')]
        target=k.OpenProcess(0x100000|0x1000,False,pid);assert target
        client.event('initialized')
        if debugger.mode=='warm':
            root=debugger.states[debugger.root_pid];child=debugger.states[pid]
            assert set(root.events)==set(child.events)=={0,1,2} and root.names==child.names
            assert root.job and root.child_handle
            assert client.send('disconnect')['success'];client.close()
            assert not debugger.commit_observed and not debugger.faults
            return {'warmup':'real debug setup and clean disconnect; no release'}
        paused=debugger.mode in ('query_reject','set_reject')
        fixture=args.paused_fixture if paused else args.streams_fixture
        assert client.send('launch',{'program':fixture.resolve().as_posix()})['success']
        if paused:
            assert client.send('setFunctionBreakpoints',{'breakpoints':[{'name':'worker'}]})['body']['breakpoints'][0]['verified']
        assert client.send('configurationDone')['success']
        if paused:
            stopped=client.event('stopped');thread=stopped['body']['threadId']
            frames=client.send('stackTrace',{'threadId':thread})['body']['stackFrames']
            threads=client.send('threads')['body']['threads'];assert len(threads)>=2
            scope=client.send('scopes',{'frameId':frames[0]['id']})['body']['scopes'][0]['variablesReference']
            variables=client.send('variables',{'variablesReference':scope})['body']['variables']
            assert frames and frames[0]['name']=='worker'
        else:
            for category,text in [('stdout','前 out\0α'),('stdout','true\n'),('stderr','前 err\0α'),('stdout','true\n')]:
                assert client.event('output')['body']=={'category':category,'output':text}
            assert paths[0].read_bytes()==PRE and paths[1].read_bytes()==ERR_PRE
        debugger.armed=True
        detached=debugger.mode in ('postcommit_death','ack_fail','close_process','close_job')
        if paused:
            failed=client.send('disconnect',{'terminateDebuggee':False});assert not failed['success'],failed
            assert len(debugger.faults)==1 and not debugger.commit_observed
            assert client.process.poll() is None and k.WaitForSingleObject(target,0)==258
            assert client.send('stackTrace',{'threadId':thread})['body']['stackFrames']==frames
            assert client.send('threads')['body']['threads']==threads
            assert client.send('variables',{'variablesReference':scope})['body']['variables']==variables
            assert paths[0].read_bytes()==b'' and paths[1].read_bytes()==b''
            assert client.send('disconnect',{'terminateDebuggee':False})['success'];detached=True
        elif detached:
            assert client.send('disconnect',{'terminateDebuggee':False})['success']
        else:
            client.seq+=1
            client.input.write(wire({'seq':client.seq,'type':'request','command':'disconnect','arguments':{'terminateDebuggee':False}}))
            client.input.flush()
        if detached:client.event('terminated')
        end=client.messages.get(timeout=20)
        if isinstance(end,BaseException):raise end
        assert end is None,end
        client.reader.join(timeout=2);assert not client.reader.is_alive()
        client.process.wait(timeout=20)
        frontend=(91 if debugger.killed else 1 if debugger.mode in ('ack_fail','close_process','close_job') else 0) if detached else (91 if debugger.killed else 0 if debugger.commit_observed else 1)
        assert client.process.returncode==frontend,(debugger.mode,client.process.returncode,frontend,debugger.trace)
        if detached and not paused:
            assert k.WaitForSingleObject(target,0)==258,'released original target ended with frontend'
            assert paths[0].read_bytes()==PRE and paths[1].read_bytes()==ERR_PRE
        assert k.WaitForSingleObject(target,20000)==0
        status=U32();assert k.GetExitCodeProcess(target,C.byref(status))
        if detached:assert status.value==0,(debugger.mode,status.value)
        else:
            # Fatal control I/O can finish in the worker or be revoked by its
            # still-owning frontend. Neither path can become target success.
            assert status.value in (1,0xe0000001),(debugger.mode,status.value)
        stdout=(b'33\n' if paused else ALL) if detached else PRE
        stderr=b'' if paused else ERR_ALL if detached else ERR_PRE
        assert paths[0].read_bytes()==stdout and paths[1].read_bytes()==stderr
        if debugger.mode=='postcommit_death':assert debugger.killed and debugger.commit_observed and not debugger.faults
        elif debugger.mode=='reject_ack':assert len(debugger.faults)==2
        else:assert len(debugger.faults)==1,debugger.faults
        diagnostic=client.process.stderr.read()
        host_diagnostic=(paths[0].parent/'host.log').read_bytes()
        for marker in (b'ERROR: AddressSanitizer',b'runtime error:',b'UndefinedBehaviorSanitizer',b'Assertion failed'):
            assert marker not in diagnostic and marker not in host_diagnostic,(marker,diagnostic,host_diagnostic)
        if frontend==1:assert b'DAP: protocol, source admission, resource or cleanup failure' in diagnostic
        else:assert not diagnostic,diagnostic
        return {'retained':retained,'frontend_exit':frontend,'target_exit':status.value,'stdout_hex':stdout.hex(),'stderr_hex':stderr.hex(),
                'snapshot_and_live_children_preserved_on_rejection':paused,'transcript':client.transcript}
    finally:
        if target:assert k.CloseHandle(target)
        if not client.input.closed:client.input.close()

record={'state':'RUNNING','full_p16_qualified':False,'mode':'ALL_OS_HANDSHAKE' if args.resources else args.case,
        'binary_sha256':hashlib.sha256(args.xray.read_bytes()).hexdigest(),'cases':[]}
try:
    warm=WinDebugger(args.xray,'warm');cold=warm.handles();record['warmup']=warm.run(exercise);baseline=warm.handles()
    baseline_handles=warm.handle_snapshot();assert warm.handles()==baseline
    for mode in MODES if args.resources else (args.case,) if args.case!='warm' else ():
        debugger=WinDebugger(args.xray,mode);result=debugger.run(exercise)
        record['cases'].append(result)
        current=debugger.handles()
        if current!=baseline:
            actual=debugger.handle_snapshot()
            record['handle_diagnostic']={'before':baseline,'after':current,'added':{handle:row for handle,row in actual.items() if handle not in baseline_handles},
                                         'removed':{handle:row for handle,row in baseline_handles.items() if handle not in actual}}
        assert current==baseline,(mode,'debugger handle imbalance',baseline,current)
        print(json.dumps({key:value for key,value in result.items() if key!='outcome'}))
    record.update(state='PASS',handle_samples=[cold,baseline,warm.handles()],full_os_sweep_executed=args.resources)
    assert hashlib.sha256(args.xray.read_bytes()).hexdigest()==record['binary_sha256']
    print('Actual CLI Windows handoff, original Source/PID/output, exact debug-handle balance: PASS')
except BaseException as error:
    record['state']='FAIL';record['error']=repr(error)
    if 'warm' in globals():record['warm_debug_trace']=warm.trace
    raise
finally:
    args.transcript.parent.mkdir(parents=True,exist_ok=True)
    args.transcript.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
