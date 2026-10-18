"""Windows target ownership and retained output through the real CLI consumer."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import json
import os
import shutil
import tempfile

def private_output(kernel,paths):
    security=ctypes.WinDLL('advapi32',use_last_error=True)
    kernel.GetCurrentProcess.restype=wintypes.HANDLE
    security.OpenProcessToken.argtypes=(wintypes.HANDLE,wintypes.DWORD,ctypes.POINTER(wintypes.HANDLE))
    security.GetTokenInformation.argtypes=(wintypes.HANDLE,ctypes.c_int,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD))
    security.GetFileSecurityW.argtypes=(wintypes.LPCWSTR,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD))
    security.GetSecurityDescriptorControl.argtypes=(ctypes.c_void_p,ctypes.POINTER(wintypes.WORD),ctypes.POINTER(wintypes.DWORD))
    security.GetSecurityDescriptorDacl.argtypes=(ctypes.c_void_p,ctypes.POINTER(wintypes.BOOL),ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(wintypes.BOOL))
    security.GetAce.argtypes=(ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(ctypes.c_void_p))
    security.EqualSid.argtypes=(ctypes.c_void_p,ctypes.c_void_p)
    token=wintypes.HANDLE();assert security.OpenProcessToken(kernel.GetCurrentProcess(),8,ctypes.byref(token))
    try:
        size=wintypes.DWORD()
        security.GetTokenInformation(token,1,None,0,ctypes.byref(size));assert size.value
        user=ctypes.create_string_buffer(size.value)
        assert security.GetTokenInformation(token,1,user,len(user),ctypes.byref(size))
        sid=ctypes.cast(user,ctypes.POINTER(ctypes.c_void_p))[0]
        for path in [paths[0].parent,*paths]:
            size=wintypes.DWORD();security.GetFileSecurityW(str(path),4,None,0,ctypes.byref(size));assert size.value
            descriptor=ctypes.create_string_buffer(size.value)
            assert security.GetFileSecurityW(str(path),4,descriptor,len(descriptor),ctypes.byref(size))
            control=wintypes.WORD();revision=wintypes.DWORD()
            assert security.GetSecurityDescriptorControl(descriptor,ctypes.byref(control),ctypes.byref(revision))
            if path==paths[0].parent:assert control.value&0x1000,'session directory DACL is not protected'
            present=wintypes.BOOL();defaulted=wintypes.BOOL();acl=ctypes.c_void_p()
            assert security.GetSecurityDescriptorDacl(descriptor,ctypes.byref(present),ctypes.byref(acl),ctypes.byref(defaulted))
            assert present.value and acl.value
            assert int.from_bytes(ctypes.string_at(acl.value+4,2),'little')==1,'unexpected additional access grant'
            ace=ctypes.c_void_p();assert security.GetAce(acl,0,ctypes.byref(ace))
            assert ctypes.string_at(ace,1)==b'\x00' and security.EqualSid(sid,ace.value+8)
    finally:assert kernel.CloseHandle(token)
    print('Actual protected directory and both inherited file DACLs grant only the current token user: PASS')

def stdio_worker(Client,args):
    mode=args.scenario.removeprefix('worker-');original_fixture=args.fixture
    def create_client():
        if mode!='unicode':return Client(read_until='output' if mode=='output-closed' else None)
        parent=args.xray.parent.resolve()
        owned=Path(tempfile.mkdtemp(prefix='stdio-工作 空间-',dir=parent)).resolve()
        assert owned.parent==parent
        args.fixture=owned/'源码 带空格.xr';shutil.copyfile(original_fixture,args.fixture)
        temporary=owned
        for index in range(4):temporary=temporary/(str(index)+'-临时目录'+'x'*24)
        temporary.mkdir(parents=True)
        previous={key:os.environ.get(key) for key in ('TMP','TEMP')}
        for key in previous:os.environ[key]=str(temporary)
        try:return Client()
        finally:
            for key,value in previous.items():
                if value is None:del os.environ[key]
                else:os.environ[key]=value
    api=ctypes.WinDLL('kernel32',use_last_error=True)
    api.OpenProcess.argtypes=(wintypes.DWORD,wintypes.BOOL,wintypes.DWORD)
    api.OpenProcess.restype=wintypes.HANDLE
    api.WaitForSingleObject.argtypes=(wintypes.HANDLE,wintypes.DWORD)
    api.GetExitCodeProcess.argtypes=(wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD))
    api.CloseHandle.argtypes=(wintypes.HANDLE,)
    client=create_client();target=None
    try:
        reply=client.send('initialize');assert reply['success'],reply
        assert reply['body']['supportTerminateDebuggee']
        retained=reply['body']['xrayRetainedOutput']
        print(json.dumps({'scenario':mode,'frontendId':client.process.pid,'retained':retained},ensure_ascii=False))
        pid=retained['processId'];assert pid>0 and pid!=client.process.pid
        target=api.OpenProcess(0x100000|0x1000,False,pid);assert target,ctypes.get_last_error()
        paths=[Path(retained[key]) for key in ('stdout','stderr')]
        assert paths[0]!=paths[1] and all(p.is_absolute() and p.is_file() for p in paths),retained
        assert paths[0].parent==paths[1].parent
        if mode in ('streams','unicode'):private_output(api,paths)
        client.event('initialized')
        if mode=='unicode':assert all(len(str(p))>260 and '工作 空间' in str(p) for p in paths)
        launch={'program':args.fixture.resolve().as_posix()}
        if mode in ('stopped','running','attach','frontend-killed','terminate'):launch['stopOnEntry']=True
        assert client.send('attach' if mode=='attach' else 'launch',launch)['success']
        if mode!='unstarted':
            assert client.send('configurationDone')['success']
            if launch.get('stopOnEntry'):
                stopped=client.event('stopped');assert stopped['body']['reason']=='entry'
                assert client.send('stackTrace',{'threadId':stopped['body']['threadId']})['body']['stackFrames']
            elif mode in ('timer','output-closed','output-normal','unicode','timer-killed'):
                first=client.event('output');assert first['body']=={'category':'stdout','output':'before 前\0α\n'},first
                assert paths[0].read_bytes()=='before 前\0α\n'.encode()
            elif mode=='streams':
                before=[('stdout','前 out\0α'),('stdout','true\n'),('stderr','前 err\0α'),('stdout','true\n')]
                for category,text in before:
                    assert client.event('output')['body']=={'category':category,'output':text}
                assert paths[0].read_bytes()=='前 out\0αtrue\ntrue\n'.encode()
                assert paths[1].read_bytes()=='前 err\0α'.encode()
        if mode in ('frontend-killed','timer-killed'):
            client.process.kill();client.process.wait(timeout=20)
            assert api.WaitForSingleObject(target,20000)==0,'unreleased worker survived frontend death'
            expected='before 前\0α\n'.encode() if mode=='timer-killed' else b''
            assert paths[0].read_bytes()==expected and paths[1].read_bytes()==b''
            print('Actual frontend kill before handoff revokes the stopped worker: PASS');return
        if mode=='terminate':
            assert client.send('disconnect',{'terminateDebuggee':True})['success']
            client.event('terminated');client.close()
            assert api.WaitForSingleObject(target,20000)==0
            assert paths[0].read_bytes()==b'' and paths[1].read_bytes()==b''
            print('Explicit terminate retains no executing worker or target output: PASS');return
        if mode=='output-normal':
            assert client.event('output')['body']=={'category':'stdout','output':'after 后\0β\n'}
            client.event('exited');client.event('terminated')
            assert client.send('disconnect')['success'];client.close()
        elif mode=='output-closed':
            client.reader.join(timeout=2);assert not client.reader.is_alive()
            client.output.close()
            client.seq+=1
            body={'seq':client.seq,'type':'request','command':'disconnect','arguments':{'terminateDebuggee':False}}
            payload=json.dumps(body,separators=(',',':')).encode()
            client.input.write(f'Content-Length: {len(payload)}\r\n\r\n'.encode()+payload);client.input.flush()
            client.process.wait(timeout=20)
            assert client.process.returncode==0,client.process.stderr.read()
        else:
            if mode=='running':assert client.send('continue',{'threadId':stopped['body']['threadId']})['success']
            policy=None if mode=='attach' else {'terminateDebuggee':False}
            assert client.send('disconnect',policy)['success']
            client.event('terminated')
            end=client.messages.get(timeout=20)
            if isinstance(end,BaseException):raise end
            assert end is None,end
            client.reader.join(timeout=2);assert not client.reader.is_alive()
            client.process.wait(timeout=20);assert client.process.returncode==0,client.process.stderr.read()
            if mode in ('timer','unicode','streams'):
                assert api.WaitForSingleObject(target,0)==258,'target ended with frontend'
                prefix='前 out\0αtrue\ntrue\n'.encode() if mode=='streams' else 'before 前\0α\n'.encode()
                assert paths[0].read_bytes()==prefix,'target timer was replayed or resumed early'
        assert api.WaitForSingleObject(target,20000)==0,'released target did not finish'
        code=wintypes.DWORD();assert api.GetExitCodeProcess(target,ctypes.byref(code))
        assert code.value==(1 if mode=='output-closed' else 0),code.value
        expected=(b'owned\0value\n' if mode=='unstarted' else b'33\n' if mode in ('stopped','running','attach')
                  else 'before 前\0α\nafter 后\0β\n'.encode())
        if mode=='streams':expected='前 out\0αtrue\ntrue\n后 out\0βtrue\ntrue\n'.encode()
        assert paths[0].read_bytes()==expected,(paths[0].read_bytes(),expected)
        expected_error='前 err\0α后 err\0β'.encode() if mode=='streams' else b''
        assert paths[1].read_bytes()==expected_error
        print(json.dumps({'targetExit':code.value,'frontendExit':client.process.returncode,
                          'stdoutHex':paths[0].read_bytes().hex(),'stderrHex':paths[1].read_bytes().hex()}))
        if mode not in ('output-closed','output-normal'):
            assert not any(e['event'] in ('output','exited') for e in client.events),client.events
        print(f'Windows stdio {mode}: distinct actual PID, original execution/output and independent endpoint lifetime PASS')
    finally:
        args.fixture=original_fixture
        if client.process.poll() is None:
            client.process.kill();client.process.wait(timeout=20)
        if target:
            if api.WaitForSingleObject(target,20000)!=0:raise AssertionError('worker cleanup did not finish')
            assert api.CloseHandle(target)
        for stream in (client.process.stdin,client.process.stdout,client.process.stderr):
            if not stream.closed:stream.close()
