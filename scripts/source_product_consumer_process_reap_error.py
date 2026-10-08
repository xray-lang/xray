#!/usr/bin/env python3
"""Exercise native Windows wait errors after damaging one owned child handle."""
import ctypes
import hashlib
import os
from pathlib import Path
import re
import subprocess
import time


def exercise(runner, helpers, command, label, run, cwd):
    if os.name != 'nt':
        raise RuntimeError('native Windows handle-error probe requires Windows')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    native_popen = subprocess.Popen
    children = []

    class HandleErrorProcess(native_popen):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self.recovery_handle = None
            self.damage_pending = True
            self.native_errors = []
            self.injection = {'pid': self.pid, 'original_handle_closed': False}
            self.output_file = Path(kwargs['stdout'].name)
            self.error_file = Path(kwargs['stderr'].name)
            children.append(self)
            # Waiting for terminal state also queries the real process exit code.
            # Own synchronization, termination, and limited-query access together.
            self.recovery_handle = kernel.OpenProcess(0x00101001, False, self.pid)
            if not self.recovery_handle:
                raise ctypes.WinError(ctypes.get_last_error())

        def wait(self, timeout=None):
            if self.damage_pending:
                self.damage_pending = False
                deadline = time.monotonic() + min(3.0, max(0.0, timeout or 0.0))
                while True:
                    out = self.output_file.read_text(encoding='utf8')
                    err = self.error_file.read_text(encoding='utf8')
                    ready = re.fullmatch(r'process-lifecycle ready=1 mode=timeout product-verified=1 session-dead=1 physical=([1-9][0-9]*)/([1-9][0-9]*)\n', out)
                    if ready and err == 'process-lifecycle stderr-ready mode=timeout\n':
                        self.injection['ready_physical'] = [int(v) for v in ready.groups()]
                        self.injection['stdout_before_error_sha256'] = hashlib.sha256(self.output_file.read_bytes()).hexdigest()
                        self.injection['stderr_before_error_sha256'] = hashlib.sha256(self.error_file.read_bytes()).hexdigest()
                        break
                    if time.monotonic() >= deadline or super().poll() is not None:
                        raise RuntimeError('actual product readiness did not precede handle damage')
                    time.sleep(0.01)
                self._handle.Close()
                self.injection['original_handle_closed'] = self._handle.closed
            try:
                # CPython delegates to the real Windows WaitForSingleObject.
                return super().wait(timeout=timeout)
            except OSError as error:
                self.native_errors.append({'type': type(error).__name__, 'winerror': error.winerror,
                                           'message': str(error)})
                raise

    receipt = None
    probe_error = None
    recoveries = []
    try:
        subprocess.Popen = HandleErrorProcess
        receipt = runner.process_run(command, label, run, time.monotonic() + 20, helpers, cwd)
        (run/(label+'.failed-before-recovery.json')).write_bytes((run/(label+'.json')).read_bytes())
    except Exception as error:
        probe_error = error
    finally:
        subprocess.Popen = native_popen
        # Repair only the dedicated child object after the runner has recorded
        # its unobserved termination. That failed receipt is never overwritten.
        for child in children:
            recovery = {'pid': child.pid, 'status': 'RUNNING', 'injection': child.injection,
                        'native_wait_errors': list(child.native_errors)}
            recoveries.append(recovery)
            try:
                if child.recovery_handle:
                    recovery['before_recovery_wait'] = kernel.WaitForSingleObject(child.recovery_handle, 0)
                    child._handle = subprocess.Handle(child.recovery_handle)
                    child.recovery_handle = None
                child.damage_pending = False
                child.kill()
                recovery['returncode'] = child.wait(timeout=8)
                recovery['termination_observed'] = True
                recovery['status'] = 'PASS'
            except Exception as error:
                recovery['status'] = 'FAIL'
                recovery['error'] = repr(error)
                # The bounded fixture exits on its own after thirty seconds.
                # Keep a valid handle until its terminal state is actually seen.
                try:
                    recovery['returncode'] = child.wait(timeout=35)
                    recovery['termination_observed'] = True
                except Exception as final_error:
                    recovery['final_reap_error'] = repr(final_error)
            finally:
                if recovery.get('termination_observed'):
                    child._handle.Close()
                helpers.write(run/(label+'.recovery.json'), recoveries)
    if probe_error:
        raise probe_error
    if len(children) != 1 or len(recoveries) != 1 or recoveries[0]['status'] != 'PASS':
        raise RuntimeError('dedicated child recovery failed: ' + repr(recoveries))
    child, recovery = children[0], recoveries[0]
    if (not receipt['entered'] or receipt['pid'] != child.pid or receipt['launch_error']
            or receipt['timed_out'] or receipt['returncode'] is not None
            or receipt['termination_observed'] or not receipt['execution_error']
            or len(receipt['termination_errors']) != 2):
        raise ValueError('native wait/kill/reap failure was not preserved')
    if (not child.injection['original_handle_closed'] or recovery['before_recovery_wait'] != 258
            or [x['winerror'] for x in recovery['native_wait_errors']] != [6, 6]
            or recovery['returncode'] != 1):
        raise ValueError('real invalid-handle errors or independent recovery were not proved')
    if not all('[WinError 6]' in x for x in [receipt['execution_error'], *receipt['termination_errors']]):
        raise ValueError('actual Windows errors differ from invalid-handle failure')
    for stream in ('stdout', 'stderr'):
        raw = Path(receipt[stream]).read_bytes()
        if hashlib.sha256(raw).hexdigest() != receipt[stream+'_sha256'] or receipt[stream+'_sha256'] != child.injection[stream+'_before_error_sha256']:
            raise ValueError('first output changed during external recovery')
    if (run/(label+'.json')).read_bytes() != (run/(label+'.failed-before-recovery.json')).read_bytes():
        raise ValueError('external recovery overwrote the runner failure')
    try:
        runner.process_contract(receipt)
    except ValueError as error:
        return {'receipt': receipt, 'process_contract': 'FAIL', 'preserved_failure': str(error),
                'ready_physical': child.injection['ready_physical'], 'recovery': recovery,
                'scope': 'Real Windows invalid-handle fault injection;no manufactured OSError or simulated process'}
    raise ValueError('runner accepted native unobserved termination as success')
