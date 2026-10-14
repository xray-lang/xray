"""Prove VM runs cannot launch even an absolute-path host C provider on Windows."""
from __future__ import annotations
import ctypes,json,os,subprocess,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'lib'))
from xraytest import windows_owned_job as owned_job


class _Accounting(ctypes.Structure):
    _fields_=[('user',ctypes.c_longlong),('kernel',ctypes.c_longlong),
              ('period_user',ctypes.c_longlong),('period_kernel',ctypes.c_longlong),
              ('faults',ctypes.c_uint32),('total',ctypes.c_uint32),
              ('active',ctypes.c_uint32),('terminated',ctypes.c_uint32)]


class NoChildProcess:
    """The test parent and its one CLI occupy both permitted Job process slots.

    Membership is inherited at CreateProcess; no post-launch assignment race or
    BREAKAWAY is allowed. Only this test's Job limits change. Native build calls
    restore its ordinary child allowance after the preceding CLI has drained.
    """
    def __init__(self,root:Path,output:Path):
        self.root,self.output=root,output
        self.supported=os.name=='nt';self.observations=[]
        self.control={'status':'NOT_SUPPORTED_ON_THIS_PLATFORM','absolute_child_denied':False}
        if not self.supported:return
        self.kernel=owned_job._kernel32();self.job=owned_job.establish_job()
        self.kernel.QueryInformationJobObject.argtypes=[ctypes.c_void_p,ctypes.c_int,
            ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p]
        self.kernel.QueryInformationJobObject.restype=ctypes.c_int
        before=self.accounting()
        assert before['active']==1,before
        program=('import json,subprocess,sys\n'
                 'try:\n'
                 ' subprocess.run([sys.executable,"-c","print(731)"],check=True)\n'
                 'except OSError as error:\n'
                 ' assert error.winerror==1816,error\n'
                 ' print(json.dumps({"absolute_child_denied":True,"winerror":error.winerror}))\n'
                 'else: raise RuntimeError("Absolute-path child execution was not denied")\n')
        argv=[sys.executable,'-c',program]
        result=self.run('absolute-child-denial-control',argv,dict(os.environ))
        (output/'no-child-control.stdout').write_bytes(result.stdout)
        (output/'no-child-control.stderr').write_bytes(result.stderr)
        assert result.returncode==0 and not result.stderr,result
        observation=json.loads(result.stdout)
        assert observation=={'absolute_child_denied':True,'winerror':1816},observation
        self.control=dict(status='PASS',argv=argv,exit_code=result.returncode,**observation,
                          job_process_limit=2,parent_slots=1,CLI_slots=1)
        (output/'no-child-control.json').write_text(json.dumps(self.control,indent=2)+'\n',encoding='utf-8')

    def accounting(self):
        state=_Accounting()
        if not self.kernel.QueryInformationJobObject(self.job,1,ctypes.byref(state),ctypes.sizeof(state),None):
            raise ctypes.WinError(ctypes.get_last_error())
        return dict(active=state.active,total=state.total,terminated=state.terminated)

    def limits(self,restricted):
        limits=owned_job._ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags=0x2000 | (0x8 if restricted else 0)
        limits.BasicLimitInformation.ActiveProcessLimit=2 if restricted else 0
        if not self.kernel.SetInformationJobObject(self.job,9,ctypes.byref(limits),ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())

    def run(self,name,argv,environment):
        if not self.supported:
            return subprocess.run(argv,cwd=self.root,env=environment,capture_output=True,timeout=60)
        before=self.accounting();assert before['active']==1,before
        self.limits(True)
        try:
            result=subprocess.run(argv,cwd=self.root,env=environment,capture_output=True,timeout=60)
            deadline=time.monotonic()+1
            after=self.accounting()
            while after['active']!=1 and time.monotonic()<deadline:
                time.sleep(0.01);after=self.accounting()
            assert after['active']==1,after
            self.observations.append(dict(name=name,argv=argv,exit_code=result.returncode,
                job_process_limit=2,CLI_descendants_allowed=0,before=before,after=after))
            (self.output/'no-child-processes.json').write_text(json.dumps(self.observations,indent=2)+'\n',encoding='utf-8')
            return result
        finally:
            # The sole Job handle belongs to this parent until its death. Do
            # not close a KILL_ON_JOB_CLOSE Job containing the running parent.
            self.limits(False)
