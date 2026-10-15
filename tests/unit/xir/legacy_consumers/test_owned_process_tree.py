"""Verify the shared timeout runner terminates its real parent and child."""
from pathlib import Path
import argparse
import json
import os
import signal
import sys
import unittest

proc=None


def configure(source_root):
    global proc
    source_root=Path(source_root).resolve(strict=True)
    sys.path.insert(0,str(source_root/'tests/lib'))
    from xraytest import proc as shared_proc
    if Path(shared_proc.__file__).resolve()!=source_root/'tests/lib/xraytest/proc.py':
        raise ValueError('Process lifetime control loaded a different shared runner')
    proc=shared_proc


def running(pid,terminate=False):
    if os.name!='nt':
        try:os.kill(pid,signal.SIGKILL if terminate else 0)
        except ProcessLookupError:return False
        return not terminate
    import ctypes
    from ctypes import wintypes
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    kernel.OpenProcess.restype=wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD]
    kernel.WaitForSingleObject.restype=wintypes.DWORD
    kernel.CloseHandle.argtypes=[wintypes.HANDLE]
    handle=kernel.OpenProcess(0x100000|(1 if terminate else 0),False,pid)
    if not handle:
        error=ctypes.get_last_error()
        if error==87:return False
        raise OSError(error,'Cannot inspect the owned test process')
    try:
        state=kernel.WaitForSingleObject(handle,0)
        if state==0:return False
        if state!=258:raise OSError(ctypes.get_last_error(),'Owned process wait failed')
        if terminate:
            kernel.TerminateProcess.argtypes=[wintypes.HANDLE,wintypes.UINT]
            kernel.TerminateProcess.restype=wintypes.BOOL
            if not kernel.TerminateProcess(handle,1):raise OSError(ctypes.get_last_error(),'Owned test process cleanup failed')
            if kernel.WaitForSingleObject(handle,5000)!=0:raise RuntimeError('Owned process did not terminate')
            return False
        return True
    finally:kernel.CloseHandle(handle)


class OwnedProcessTree(unittest.TestCase):
    def test_timeout_terminates_real_descendant_and_preserves_output(self):
        self.assertIsNotNone(proc)
        child="import os,time;print(os.getpid(),flush=True);time.sleep(60)"
        parent=("import os,subprocess,sys,time,json;"
                "child=subprocess.Popen([sys.executable,'-c',"+repr(child)+"],stdout=subprocess.PIPE,text=True);"
                "pid=int(child.stdout.readline());"
                "print(json.dumps({'parent':os.getpid(),'child':pid}),flush=True);time.sleep(60)")
        observed=proc.run([sys.executable,'-c',parent],timeout=5)
        pids=json.loads(observed.stdout)
        self.assertEqual(set(pids),{'parent','child'})
        self.assertTrue(all(type(pid) is int and pid>0 and pid!=os.getpid() for pid in pids.values()))
        self.assertNotEqual(pids['parent'],pids['child'])
        try:
            self.assertTrue(observed.timed_out)
            self.assertNotEqual(observed.returncode,0)
            self.assertEqual(observed.stderr,b'')
            self.assertFalse(running(pids['parent']))
            self.assertFalse(running(pids['child']))
            print(json.dumps({'scope':'SHARED_TEST_PROCESS_LIFETIME_ONLY','parent':pids['parent'],'child':pids['child'],
                'timed_out':True,'both_processes_terminated':True,'compiler_results_mocked':False}),flush=True)
        finally:
            for pid in pids.values():
                if running(pid):running(pid,terminate=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,required=True)
    args=parser.parse_args();configure(args.source_root)
    unittest.main(argv=[sys.argv[0]])
