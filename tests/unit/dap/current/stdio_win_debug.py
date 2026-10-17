"""Observe actual owned CLI processes with Win64 CPU breakpoints and bounded API faults.

The creator thread drains every debug event. Kernel debug-event process/thread
handles are closed by ContinueDebugEvent; image-file handles belong to this loop.
A temporary entry breakpoint is restored before application execution. Thereafter
CPU breakpoints observe actual APIs; executable files and other processes are unchanged.
"""
import ctypes as C
import struct
import subprocess
import threading
import time
from pathlib import Path

U64=C.c_uint64;U32=C.c_uint32;U16=C.c_uint16;PTR=C.c_void_p

class Context64(C.Structure):
    _fields_=[('home',U64*6),('ContextFlags',U32),('MxCsr',U32),('segments',U16*6),('EFlags',U32),
              *[(name,U64) for name in ('Dr0','Dr1','Dr2','Dr3','Dr6','Dr7','Rax','Rcx','Rdx','Rbx','Rsp',
                 'Rbp','Rsi','Rdi','R8','R9','R10','R11','R12','R13','R14','R15','Rip')],('tail',C.c_ubyte*976)]

class AlignedContext:
    def __init__(self):
        self.storage=C.create_string_buffer(C.sizeof(Context64)+15)
        self.value=Context64.from_address((C.addressof(self.storage)+15)&~15)
        self.value.ContextFlags=0x100013

class ExceptionRecord(C.Structure):
    _fields_=[('code',U32),('flags',U32),('record',PTR),('address',PTR),('count',U32),('information',U64*15)]
class ExceptionInfo(C.Structure):_fields_=[('record',ExceptionRecord),('first',U32)]
class CreateInfo(C.Structure):
    _fields_=[('file',PTR),('process',PTR),('thread',PTR),('base',PTR),('offset',U32),('size',U32),
              ('tls',PTR),('start',PTR),('name',PTR),('unicode',U16)]
class DllInfo(C.Structure):_fields_=[('file',PTR),('base',PTR),('offset',U32),('size',U32),('name',PTR),('unicode',U16)]
class DebugUnion(C.Union):_fields_=[('exception',ExceptionInfo),('create',CreateInfo),('dll',DllInfo),('exit',U32),('raw',C.c_ubyte*160)]
class DebugEvent(C.Structure):_fields_=[('code',U32),('pid',U32),('tid',U32),('data',DebugUnion)]
class SnapshotHandle(C.Structure):
    _fields_=[('handle',PTR),('flags',U32),('kind',U32),('capture',U32*2),('basic',U32*6),('created',U32*2),
              ('type_length',U16),('type_name',PTR),('name_length',U16),('name',PTR),('specific',C.c_ubyte*48)]

assert C.sizeof(Context64)==1232 and Context64.Rax.offset==120 and Context64.Rsp.offset==152 and Context64.Rip.offset==248
assert C.sizeof(ExceptionRecord)==152 and C.sizeof(DebugEvent)==176 and DebugEvent.data.offset==16
assert C.sizeof(SnapshotHandle)==136 and SnapshotHandle.specific.offset==88

class ImportTable:
    def __init__(self,path):
        data=path.read_bytes();assert len(data)<134217728 and data[:2]==b'MZ'
        u16=lambda p:struct.unpack_from('<H',data,p)[0]
        u32=lambda p:struct.unpack_from('<I',data,p)[0]
        nt=u32(60);assert data[nt:nt+4]==b'PE\0\0' and u16(nt+4)==0x8664
        opt=nt+24;assert u16(opt)==0x20b
        sections=opt+u16(nt+20);self.size=u32(opt+56);self.entry=u32(opt+16)
        def offset(rva):
            if rva<u32(opt+60):return rva
            for i in range(u16(nt+6)):
                section=sections+i*40;start=u32(section+12);size=u32(section+16)
                if start<=rva<start+size:return u32(section+20)+rva-start
            raise AssertionError(('unmapped import RVA',rva))
        def string(rva):
            start=offset(rva);end=data.find(b'\0',start,start+256);assert end>=start
            return data[start:end].decode('ascii')
        self.thunks={};descriptor=offset(u32(opt+120))
        for n in range(1024):
            lookup,stamp,chain,name,first=struct.unpack_from('<IIIII',data,descriptor+n*20)
            if not any((lookup,stamp,chain,name,first)):break
            for i in range(65536):
                entry=struct.unpack_from('<Q',data,offset((lookup or first)+i*8))[0]
                if not entry:break
                if not entry>>63:self.thunks[string(entry+2)]=first+i*8
            else:raise AssertionError('unterminated import thunk table')
        else:raise AssertionError('unterminated import directory')

class ProcessState:
    def __init__(self,pid,info,root):
        self.pid=pid;self.handle=info.process;self.thread=info.thread;self.base=info.base;self.root=root
        self.initial=False;self.entry_pending=False;self.entry_byte=None;self.is_xray=True
        self.events={};self.names={};self.pending={};self.job=None;self.child_handle=None
        self.apis={};self.slots=[];self.committed=False;self.closed=[];self.ack=False
        self.flushes=0;self.resets=0

class WinDebugger:
    def __init__(self,path,mode):
        assert C.sizeof(PTR)==8
        self.kernel=C.WinDLL('kernel32',use_last_error=True);self.imports=ImportTable(path);self.path=path
        self.mode=mode;self.process=None;self.states={};self.thread_ids={};self.exits={};self.trace=[];self.armed=False;self.auxiliary=[]
        self.faults=[];self.root_pid=None;self.worker_pid=None;self.killed=False;self.commit_observed=False
        k=self.kernel
        k.WaitForDebugEvent.argtypes=(C.POINTER(DebugEvent),U32)
        k.ContinueDebugEvent.argtypes=(U32,U32,U32)
        k.GetThreadContext.argtypes=(PTR,PTR);k.SetThreadContext.argtypes=(PTR,PTR)
        k.ReadProcessMemory.argtypes=(PTR,PTR,PTR,C.c_size_t,C.POINTER(C.c_size_t))
        k.WriteProcessMemory.argtypes=(PTR,PTR,PTR,C.c_size_t,C.POINTER(C.c_size_t))
        k.FlushInstructionCache.argtypes=(PTR,PTR,C.c_size_t)
        k.CloseHandle.argtypes=(PTR,);k.TerminateProcess.argtypes=(PTR,U32)
        k.GetCurrentProcess.restype=PTR
        k.GetProcessId.argtypes=(PTR,);k.GetProcessId.restype=U32
        k.QueryFullProcessImageNameW.argtypes=(PTR,U32,PTR,C.POINTER(U32))
        k.GetSystemDirectoryW.argtypes=(PTR,U32);k.GetSystemDirectoryW.restype=U32
        k.VirtualQueryEx.argtypes=(PTR,PTR,PTR,C.c_size_t);k.VirtualQueryEx.restype=C.c_size_t
        k.GetProcessHandleCount.argtypes=(PTR,C.POINTER(U32))
    def check(self,result,label):
        if not result:raise OSError(C.get_last_error(),label)
    def handles(self):
        count=U32();self.check(self.kernel.GetProcessHandleCount(self.kernel.GetCurrentProcess(),C.byref(count)),'handle count')
        return count.value
    def handle_snapshot(self):
        k=self.kernel;snapshot=PTR();marker=PTR();result={}
        k.PssCaptureSnapshot.argtypes=(PTR,U32,U32,C.POINTER(PTR));k.PssCaptureSnapshot.restype=U32
        k.PssWalkMarkerCreate.argtypes=(PTR,C.POINTER(PTR));k.PssWalkMarkerCreate.restype=U32
        k.PssWalkSnapshot.argtypes=(PTR,U32,PTR,PTR,U32);k.PssWalkSnapshot.restype=U32
        k.PssWalkMarkerFree.argtypes=(PTR,);k.PssWalkMarkerFree.restype=U32
        k.PssFreeSnapshot.argtypes=(PTR,PTR);k.PssFreeSnapshot.restype=U32
        assert k.PssCaptureSnapshot(k.GetCurrentProcess(),0x3c,0,C.byref(snapshot))==0
        try:
            assert k.PssWalkMarkerCreate(None,C.byref(marker))==0
            try:
                for unused in range(4096):
                    entry=SnapshotHandle();status=k.PssWalkSnapshot(snapshot,2,marker,C.byref(entry),C.sizeof(entry))
                    if status==259:break
                    assert status==0,status
                    typename=C.string_at(entry.type_name,entry.type_length).decode('utf-16-le').rstrip('\0') if entry.type_name else ''
                    row={'type':typename,'kind':entry.kind}
                    if entry.kind==1:row['pid']=struct.unpack_from('<I',bytes(entry.specific),28)[0]
                    if entry.kind==2:row.update(pid=struct.unpack_from('<I',bytes(entry.specific),16)[0],tid=struct.unpack_from('<I',bytes(entry.specific),20)[0])
                    result[entry.handle]=row
                else:raise AssertionError('unbounded controller handle inventory')
            finally:assert k.PssWalkMarkerFree(marker)==0
        finally:assert k.PssFreeSnapshot(k.GetCurrentProcess(),snapshot)==0
        return result
    def read(self,state,address,size):
        assert address and 0<size<=4096
        value=C.create_string_buffer(size);got=C.c_size_t()
        if not self.kernel.ReadProcessMemory(state.handle,address,value,size,C.byref(got)):
            error=C.get_last_error();region=C.create_string_buffer(48)
            region_size=self.kernel.VirtualQueryEx(state.handle,address,region,len(region))
            observed={'pid':state.pid,'handle_pid':self.kernel.GetProcessId(state.handle),'handle':state.handle,'base':hex(state.base),
                      'address':hex(address),'size':size,'region_size':region_size,'region_hex':region.raw.hex()}
            path=C.create_unicode_buffer(32768);length=U32(len(path))
            observed['image_query_ok']=bool(self.kernel.QueryFullProcessImageNameW(state.handle,0,path,C.byref(length)))
            observed['image_path']=path.value;observed['root_pid']=self.root_pid;observed['root_role']=state.root
            self.trace.append({'event':'read-diagnostic',**observed});raise OSError(error,repr(observed))
        assert got.value==size;return value.raw
    def word(self,state,address):return struct.unpack('<Q',self.read(state,address,8))[0]
    def patch_entry(self,state,value):
        count=C.c_size_t();byte=C.create_string_buffer(value)
        self.check(self.kernel.WriteProcessMemory(state.handle,state.base+self.imports.entry,byte,1,C.byref(count)),'temporary owned entry breakpoint')
        assert count.value==1
        self.check(self.kernel.FlushInstructionCache(state.handle,state.base+self.imports.entry,1),'flush owned entry instruction cache')
        assert self.read(state,state.base+self.imports.entry,1)==value
        self.trace.append({'event':'entry-byte-written-and-verified','pid':state.pid,
                           'address':hex(state.base+self.imports.entry),'byte':value.hex()})
    def text(self,state,address):
        if not address:return ''
        result=bytearray()
        for i in range(112):
            unit=self.read(state,address+i*2,2)
            if unit==b'\0\0':return result.decode('utf-16-le')
            result+=unit
        raise AssertionError('unterminated bounded event name')
    def context(self,state):
        owner=AlignedContext();self.check(self.kernel.GetThreadContext(state.thread,C.byref(owner.value)),'get owned main thread context')
        return owner
    def apply(self,state,owner):
        c=owner.value
        for index,slot in enumerate(state.slots):setattr(c,'Dr'+str(index),slot[1])
        c.Dr6=0;c.Dr7=0x55;c.EFlags|=0x10000
        self.check(self.kernel.SetThreadContext(state.thread,C.byref(c)),'set owned main thread CPU breakpoints')
    def install(self,state,entry_context):
        names=('SetInformationJobObject','AssignProcessToJobObject','SetEvent','CreateEventW') if state.root else (
            'QueryInformationJobObject','WaitForMultipleObjects' if self.mode=='wait_decision' else 'FlushFileBuffers','ResetEvent','OpenEventW')
        required=set(names)|{'QueryInformationJobObject','CloseHandle','SetEvent'}
        if state.root and self.mode in ('parent_reset','parent_wait'):required.add('ResetEvent' if self.mode=='parent_reset' else 'WaitForSingleObject')
        for name in required:
            assert name in self.imports.thunks,('missing real CLI import',name)
            self.trace.append({'event':'resolve-import','pid':state.pid,'base':hex(state.base),'api':name,
                               'iat_rva':hex(self.imports.thunks[name]),'image_size':hex(self.imports.size)})
            state.apis[name]=self.word(state,state.base+self.imports.thunks[name])
            self.trace[-1]['resolved_address']=hex(state.apis[name])
        state.slots=[(name,state.apis[name]) for name in names]
        self.apply(state,entry_context)
        actual=self.context(state).value
        self.trace.append({'event':'cpu-breakpoints-installed','pid':state.pid,'rip':hex(actual.Rip),'flags':hex(actual.ContextFlags),
                           'dr':[hex(getattr(actual,'Dr'+str(i))) for i in range(4)],'dr7':hex(actual.Dr7)})
    def later(self,state,slot,context,kind,detail):
        assert slot not in state.pending
        state.pending[slot]=(state.slots[slot],kind,detail)
        state.slots[slot]=(kind,self.word(state,context.Rsp))
    def fail(self,state,context,api,value=0,detail=None):
        self.faults.append({'pid':state.pid,'api':api,'phase':'before-effect','detail':detail})
        context.Rip=self.word(state,context.Rsp);context.Rsp+=8;context.Rax=value
    def record_return(self,state,slot,c):
        original,kind,detail=state.pending.pop(slot);state.slots[slot]=original
        if kind=='event':
            index,name=detail;assert c.Rax and index not in state.events
            state.events[index]=c.Rax;state.names[index]=name
            self.trace.append({'event':'actual-control-opened','pid':state.pid,'index':index,'name':name,'handle':c.Rax})
            if len(state.events)==3:
                replacement='CloseHandle' if state.root else 'SetEvent'
                state.slots[slot]=(replacement,state.apis[replacement])
        elif kind=='assign':
            assert c.Rax==1;state.child_handle=detail
            self.trace.append({'event':'actual-owned-group-assigned','pid':state.pid,'job':state.job,'worker_handle':detail})
            replacement='QueryInformationJobObject'
            if self.mode=='parent_reset':replacement='ResetEvent'
            if self.mode=='parent_wait':replacement='WaitForSingleObject'
            state.slots[slot]=(replacement,state.apis[replacement])
        elif kind=='policy':
            assert c.Rax==1
            if not detail&0x2000:
                state.committed=True;self.commit_observed=True
                self.trace.append({'event':'actual-job-policy-cleared','pid':state.pid,'job':state.job})
        elif kind=='close':
            assert c.Rax==1
            state.closed.append(detail)
            self.trace.append({'event':'actual-owned-handle-closed','pid':state.pid,'kind':detail})
            selected='process' if self.mode=='close_process' else 'job' if self.mode=='close_job' else None
            if detail==selected:
                assert not self.faults;c.Rax=0
                self.faults.append({'pid':state.pid,'api':'CloseHandle','phase':'after-actual-success','detail':detail})
    def breakpoint(self,state,owner):
        c=owner.value;hits=c.Dr6&15
        if not hits:return False
        assert hits&(hits-1)==0
        slot=hits.bit_length()-1;api,address=state.slots[slot];assert c.Rip==address,(api,c.Rip,address)
        if slot in state.pending:self.record_return(state,slot,c);self.apply(state,owner);return True
        armed=self.armed;already=bool(self.faults)
        if api in ('CreateEventW','OpenEventW'):
            name=self.text(state,c.R9 if api=='CreateEventW' else c.R8)
            if name.startswith('Local\\XrayDap-'):
                index=int(name[-1]);assert index in (0,1,2)
                self.later(state,slot,c,'event',(index,name))
        elif state.root and api=='AssignProcessToJobObject':
            assert state.job==c.Rcx
            self.later(state,slot,c,'assign',c.Rdx)
        elif state.root and api=='SetInformationJobObject' and c.Rdx==9:
            flags=struct.unpack('<I',self.read(state,c.R8+16,4))[0]
            if flags&0x2000:state.job=c.Rcx
            assert state.job==c.Rcx
            if armed and not flags&0x2000 and not already and self.mode in ('set_reject','reject_ack'):
                self.fail(state,c,api,detail='kill-on-close remains set')
            else:self.later(state,slot,c,'policy',flags)
        elif state.root and api=='QueryInformationJobObject' and armed and c.Rcx==state.job and c.Rdx==9:
            if self.mode=='query_reject' and not already:self.fail(state,c,api)
        elif state.root and api in ('ResetEvent','WaitForSingleObject') and armed and c.Rcx==state.events.get(0):
            if self.mode=='parent_reset' and api=='ResetEvent' and not already:self.fail(state,c,api)
            if self.mode=='parent_wait' and api=='WaitForSingleObject' and not already:self.fail(state,c,api,0xffffffff)
        elif state.root and api=='CloseHandle' and state.committed and not state.ack:
            kind='job' if c.Rcx==state.job else 'process' if c.Rcx==state.child_handle else None
            if kind:self.later(state,slot,c,'close',kind)
        elif state.root and api=='SetEvent' and armed:
            if c.Rcx==state.events.get(1):
                assert state.committed and state.closed==['job','process']
                state.ack=True;self.trace.append({'event':'before-commit-ack','pid':state.pid,'handle':c.Rcx})
                if self.mode in ('postcommit_death','query_after_parent_death'):
                    self.killed=True
                    self.check(self.kernel.TerminateProcess(state.handle,91),'terminate only the owned committed frontend')
                elif self.mode=='ack_fail' and not already:self.fail(state,c,api,detail='commit acknowledgement')
            elif c.Rcx==state.events.get(2) and self.mode=='reject_ack':
                assert len(self.faults)==1 and not state.committed
                self.fail(state,c,api,detail='rejection acknowledgement after explicit setup failure')
        elif not state.root and armed:
            if api=='FlushFileBuffers':
                state.flushes+=1;which={'flush_stdout':1,'flush_stderr':2}.get(self.mode)
                if which==state.flushes and not already:self.fail(state,c,api,detail=which)
            elif api=='ResetEvent' and c.Rcx in state.events.values():
                state.resets+=1;which={'reset_commit':1,'reset_reject':2}.get(self.mode)
                if which==state.resets and not already:self.fail(state,c,api,detail=which)
            elif api=='SetEvent' and c.Rcx==state.events.get(0) and self.mode=='signal_request' and not already:self.fail(state,c,api)
            elif api=='WaitForMultipleObjects' and self.mode=='wait_decision' and not already:
                assert c.Rcx==3 and self.word(state,c.Rdx)==state.events[1] and self.word(state,c.Rdx+8)==state.events[2]
                self.fail(state,c,api,0xffffffff)
            elif api=='QueryInformationJobObject' and self.mode=='query_after_parent_death' and self.killed and c.Rcx==0 and not already:
                self.fail(state,c,api)
        self.apply(state,owner);return True
    def run(self,exercise):
        self.process=subprocess.Popen([str(self.path),'dap','--stdio'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                      creationflags=0x00000001|0x08000000)
        self.root_pid=self.process.pid;errors=[];outcome={}
        def client():
            try:outcome.update(exercise(self))
            except BaseException as error:errors.append(error)
            finally:
                for stream in (self.process.stdin,self.process.stdout,self.process.stderr):
                    if not stream.closed:stream.close()
        worker=threading.Thread(target=client);worker.start();begin=time.monotonic();loop_error=None;pending=None
        try:
            while self.states or worker.is_alive() or not self.exits:
                assert time.monotonic()-begin<35,'owned debuggee deadline exceeded'
                event=DebugEvent()
                if not self.kernel.WaitForDebugEvent(C.byref(event),50):
                    assert C.get_last_error() in (121,258),('debug wait',C.get_last_error());continue
                pending=event
                status=0x10002
                if event.code==3:
                    info=event.data.create;state=ProcessState(event.pid,info,event.pid==self.root_pid)
                    image=C.create_unicode_buffer(32768);length=U32(len(image))
                    self.check(self.kernel.QueryFullProcessImageNameW(state.handle,0,image,C.byref(length)),'identify owned debug image')
                    state.is_xray=self.path.samefile(image.value)
                    if state.root:assert state.is_xray
                    if not state.root and state.is_xray:
                        assert self.worker_pid is None;self.worker_pid=event.pid
                    elif not state.is_xray:
                        system=C.create_unicode_buffer(32768)
                        assert self.kernel.GetSystemDirectoryW(system,len(system))
                        assert (Path(system.value)/'conhost.exe').samefile(image.value),('unexpected owned debug child',image.value)
                        self.auxiliary.append({'pid':event.pid,'image':image.value});assert len(self.auxiliary)<=4
                    self.states[event.pid]=state
                    self.trace.append({'event':'created','pid':event.pid,'root_pid':self.root_pid,'base':hex(info.base),'root_role':state.root,'image':image.value})
                    self.thread_ids[event.pid]=event.tid
                    if info.file:self.check(self.kernel.CloseHandle(info.file),'close process image debug handle')
                elif event.code==6:
                    if event.data.dll.file:self.check(self.kernel.CloseHandle(event.data.dll.file),'close DLL debug handle')
                elif event.code==1:
                    state=self.states[event.pid];exception=event.data.exception.record.code
                    if exception==0x80000003 and not state.initial:
                        assert event.tid==self.main_thread(event.pid)
                        state.initial=True
                        if state.is_xray:
                            state.entry_byte=self.read(state,state.base+self.imports.entry,1)
                            assert state.entry_byte!=b'\xcc';state.entry_pending=True;self.patch_entry(state,b'\xcc')
                    elif exception==0x80000003 and state.entry_pending:
                        assert event.tid==self.main_thread(event.pid)
                        assert event.data.exception.record.address==state.base+self.imports.entry
                        owner=self.context(state);assert owner.value.Rip==state.base+self.imports.entry+1
                        self.patch_entry(state,state.entry_byte);owner.value.Rip-=1;state.entry_pending=False
                        self.install(state,owner)
                    elif not state.is_xray:
                        status=0x80010001
                        if not event.data.exception.first:self.trace.append({'event':'unhandled-debug-exception','pid':event.pid,'code':exception})
                    elif exception==0x80000004 and state.initial and event.tid==self.main_thread(event.pid):
                        if not self.breakpoint(state,self.context(state)):status=0x80010001
                    else:
                        status=0x80010001
                        self.trace.append({'event':'unhandled-debug-exception','pid':event.pid,'code':exception})
                elif event.code==5:
                    self.exits[event.pid]=event.data.exit;del self.states[event.pid]
                self.check(self.kernel.ContinueDebugEvent(event.pid,event.tid,status),'continue actual debug event')
                pending=None
        except BaseException as error:
            loop_error=error
            for state in self.states.values():self.kernel.TerminateProcess(state.handle,92)
            if pending:self.kernel.ContinueDebugEvent(pending.pid,pending.tid,0x80010001)
            cleanup_end=time.monotonic()+10
            while self.states and time.monotonic()<cleanup_end:
                event=DebugEvent()
                if not self.kernel.WaitForDebugEvent(C.byref(event),50):continue
                if event.code==3:
                    info=event.data.create
                    self.states[event.pid]=ProcessState(event.pid,info,False)
                    if info.file:self.kernel.CloseHandle(info.file)
                    self.kernel.TerminateProcess(info.process,92)
                elif event.code==6 and event.data.dll.file:self.kernel.CloseHandle(event.data.dll.file)
                elif event.code==5:
                    self.exits[event.pid]=event.data.exit;self.states.pop(event.pid,None)
                self.kernel.ContinueDebugEvent(event.pid,event.tid,0x10002)
        worker.join(timeout=25)
        try:
            assert not worker.is_alive(),'client thread failed to terminate'
            if loop_error:raise loop_error
            if errors:raise errors[0]
            assert set(self.exits)=={self.root_pid,self.worker_pid,*[item['pid'] for item in self.auxiliary]}
            assert not any(row['event']=='unhandled-debug-exception' for row in self.trace)
            return {'mode':self.mode,'frontend':self.root_pid,'worker':self.worker_pid,'exits':self.exits,
                    'committed':self.commit_observed,'faults':self.faults,'trace':self.trace,'auxiliary':self.auxiliary,'outcome':outcome}
        finally:
            if not self.states:self.process.wait(timeout=5)
            if self.process.returncode is not None:self.process._handle.Close()
            # Closed buffered streams and Popen's wait lock retain Python
            # semaphore owners until the enclosing process object is retired.
            self.process=None
    def main_thread(self,pid):
        return self.thread_ids[pid]
