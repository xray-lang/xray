#!/usr/bin/env python3
"""Deny actual receipt writes while retaining one real child process outcome."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import time


def exercise(runner, faults, helpers, command, label, phase, run, cwd, terminal):
    if os.name != 'nt' or phase not in ('pid-write', 'final-write'):
        raise ValueError('receipt sharing probe requires Windows and an exact phase')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                   ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    invalid = ctypes.c_void_p(-1).value
    original_write = helpers.write
    receipt_file = run/(label+'.json')
    state = {'phase': phase, 'armed': False, 'file_handle': None}

    def denied_write(path, value):
        at_phase = value.get('entered') is True and ((phase == 'pid-write' and value.get('returncode') is None)
                                                    or (phase == 'final-write' and value.get('termination_observed') is True)) if isinstance(value, dict) else False
        if Path(path) == receipt_file and at_phase and not state['armed']:
            deadline = time.monotonic() + 3
            while True:
                out = Path(value['stdout']).read_text(encoding='utf8')
                err = Path(value['stderr']).read_text(encoding='utf8')
                if 'product-verified=1 session-dead=1 physical=' in out and 'stderr-ready' in err:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('real product readiness did not precede write denial')
                time.sleep(0.01)
            state['actual_before_write'] = dict(value)
            state['prior_disk_receipt'] = json.loads(receipt_file.read_text())
            state['prior_disk_sha256'] = hashlib.sha256(receipt_file.read_bytes()).hexdigest()
            # A read-only shared handle permits recovery reads but denies writers.
            handle = kernel.CreateFileW(str(receipt_file), 0x80000000, 1, None, 3, 0x80, None)
            if handle == invalid:
                raise ctypes.WinError(ctypes.get_last_error())
            state['file_handle'] = handle
            state['armed'] = True
            blocked = kernel.CreateFileW(str(receipt_file), 0x40000000, 1, None, 3, 0x80, None)
            if blocked != invalid:
                kernel.CloseHandle(blocked)
                raise RuntimeError('actual write handle unexpectedly bypassed sharing restriction')
            state['native_write_winerror'] = ctypes.get_last_error()
        return original_write(path, value)

    outcome = None
    captured_exception = None
    try:
        helpers.write = denied_write
        try:
            outcome = runner.process_run(command, label, run, time.monotonic()+20, helpers, cwd)
        except PermissionError as error:
            captured_exception = {'type': type(error).__name__, 'errno': error.errno,
                                  'winerror': error.winerror if hasattr(error, 'winerror') else None,
                                  'message': str(error)}
            outcome = faults.process_failure(command, label, run, error, helpers)
    finally:
        helpers.write = original_write
        if state['file_handle']:
            if not kernel.CloseHandle(state['file_handle']):
                raise ctypes.WinError(ctypes.get_last_error())
            state['file_handle'] = None
    if not state['armed'] or state.get('native_write_winerror') != 32 or captured_exception is None:
        raise ValueError('actual sharing violation did not produce a real PermissionError')
    pid = state['actual_before_write']['pid']
    observation = terminal(pid)
    evidence = {'state': state, 'exception': captured_exception, 'outcome': outcome,
                'actual_pid_terminal': observation}
    original_write(run/(label+'.write-denial-observation.json'), evidence)
    if hashlib.sha256(receipt_file.read_bytes()).hexdigest() != state['prior_disk_sha256']:
        raise ValueError('blocked first receipt changed')
    if (not outcome.get('entered') or outcome.get('pid') != pid or not outcome.get('termination_observed')
            or outcome.get('returncode') != (1 if phase == 'pid-write' else 0)
            or not outcome.get('receipt_write_error') or not outcome.get('runner_exception')):
        raise ValueError('write failure lost the actual PID or latest observed process outcome')
    for stream in ('stdout', 'stderr'):
        if hashlib.sha256(Path(outcome[stream]).read_bytes()).hexdigest() != outcome.get(stream+'_sha256'):
            raise ValueError('write failure lost raw output hash')
    try:
        runner.process_contract(outcome)
    except ValueError as error:
        return {'receipt': outcome, 'process_contract': 'FAIL', 'preserved_failure': str(error),
                'terminal_observation': observation, 'write_denial': evidence,
                'scope': 'Actual Windows sharing violation32;real child and original filesystem writer'}
    raise ValueError('failed receipt write was accepted as successful qualification')
