#!/usr/bin/env python3
"""Deny real input reads before launch and after a verified product process."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace
import sys
import time


class DeniedInputRead:
    def __init__(self, path):
        if os.name != 'nt':
            raise ValueError('input sharing probe requires Windows')
        self.path = Path(path)
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                          ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        self.kernel.CreateFileW.restype = ctypes.c_void_p
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel.CloseHandle.restype = ctypes.c_int
        self.handle = None
        self.evidence = {'input': str(self.path), 'native_read_winerror': None, 'released': False}

    def __enter__(self):
        invalid = ctypes.c_void_p(-1).value
        self.handle = self.kernel.CreateFileW(str(self.path), 0x80000000, 0, None, 3, 0x80, None)
        if self.handle == invalid:
            self.handle = None
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            read = self.kernel.CreateFileW(str(self.path), 0x80000000, 1, None, 3, 0x80, None)
            if read != invalid:
                self.kernel.CloseHandle(read)
                raise ValueError('actual read handle bypassed the sharing restriction')
            self.evidence['native_read_winerror'] = ctypes.get_last_error()
            if self.evidence['native_read_winerror'] != 32:
                raise ValueError('native read did not fail with a sharing violation')
            return self
        except Exception:
            self.__exit__(None, None, None)
            raise

    def __exit__(self, exception_type, exception, traceback):
        if self.handle is not None:
            if not self.kernel.CloseHandle(self.handle):
                raise ctypes.WinError(ctypes.get_last_error())
            self.handle = None
            self.evidence['released'] = True


def exercise(args, faults, axes, helpers, helper_name, normal, run, terminal):
    runner = faults if helper_name == 'faults' else axes
    label = helper_name + '-input-capture'
    source = args.source_file
    stat = source.stat()
    original = {'sha256': helpers.digest(source), 'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns}
    receipt = normal['receipt']
    product_output = Path(receipt['stdout']).read_text(encoding='utf8')
    if (normal['status'] != 'PASS' or normal['mode'] != 'normal' or receipt['returncode'] != 0
            or 'product-verified=1 session-dead=1 physical=' not in product_output
            or 'process-lifecycle released=1 mode=normal physical=0/0' not in product_output):
        raise ValueError('after-capture probe lacks an actual verified and released product')
    child_evidence = run / (label + '-initial')
    command = [sys.executable, '-B', '-X', 'utf8', str(args.input_root/'scripts'/
               ('source_product_consumer_allocation_faults.py' if helper_name=='faults' else 'source_product_consumer_allocation_axes.py'))]
    for name, value in (('binary',args.binary), ('root',args.root), ('file',args.file),
                        ('input-root',args.input_root), ('source-file',source),
                        ('registration-file',args.registration_file), ('evidence',child_evidence)):
        command += ['--'+name, str(value)]
    command += ['--kind','compiler','--jobs','8'] if helper_name=='faults' else ['--axis','allocated','--cut','exact']
    initial_lock = DeniedInputRead(source)
    with initial_lock:
        initial = faults.process_run(command, label+'-initial', run, time.monotonic()+30, helpers, args.input_root)
    if initial['returncode'] != 1 or initial['timed_out'] or not initial['termination_observed']:
        raise ValueError('initial capture denial did not fail the original runner')
    report = json.loads(Path(initial['stdout']).read_text(encoding='utf8'))
    child_runs = list(child_evidence.iterdir())
    if (report.get('status') != 'FAIL' or report.get('stage') != 'INPUT_CAPTURE'
            or report.get('product_execution') != 'NOT_RUN' or not report.get('issues')
            or repr(str(source)) not in report['issues'][0] or '[Errno 13]' not in report['issues'][0]
            or Path(initial['stderr']).read_bytes()
            or len(child_runs) != 1 or report != json.loads((child_runs[0]/'result.json').read_text())
            or {p.name for p in child_runs[0].iterdir()} != {'result.json'}):
        raise ValueError('initial capture failure lost the actual error or claimed execution')
    try:
        faults.process_contract(initial)
    except ValueError:
        pass
    else:
        raise ValueError('failed initial capture was accepted')
    capture_args = SimpleNamespace(**vars(args), axis='allocated', cut='exact')
    after_lock = DeniedInputRead(source)
    after_error = None
    with after_lock:
        try:
            runner.capture_inputs(capture_args, helpers) if helper_name=='faults' else runner.capture(capture_args, faults, helpers)
        except PermissionError as error:
            after_error = {'type': type(error).__name__, 'errno': error.errno, 'message': str(error),
                           'filename': str(error.filename)}
    if after_error is None or after_error['errno'] != 13 or Path(after_error['filename']) != source:
        raise ValueError('actual after-capture read did not raise PermissionError')
    # The failed preservation observation cannot qualify even an exit-zero product.
    after = {'status': 'FAIL', 'stage': 'AFTER_INPUT_CAPTURE', 'issues': [after_error['message']],
             'input_capture_errors': [after_error], 'changed': ['INPUT_CAPTURE_FAILED'],
             'normal_processes': [receipt], 'product_execution': 'ACTUAL_NORMAL_PRODUCT',
             'full_FI': 'NOT_RUN', 'axes': 'NOT_RUN'}
    helpers.write(run/(label+'-after.json'), after)
    final_capture = faults.capture_inputs(args, helpers)
    helpers.write(run/(label+'-restored-inputs.json'), final_capture)
    stat = source.stat()
    if original != {'sha256': helpers.digest(source), 'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns}:
        raise ValueError('denied input was modified')
    for stream in ('stdout','stderr'):
        if hashlib.sha256(Path(receipt[stream]).read_bytes()).hexdigest() != receipt[stream+'_sha256']:
            raise ValueError('after-capture failure changed original product output')
    evidence = {'initial_receipt': initial, 'initial_report': report, 'initial_lock': initial_lock.evidence,
                'initial_terminal': terminal(initial['pid']), 'after_report': after,
                'after_lock': after_lock.evidence, 'protected_input': original,
                'retained_product_terminal': terminal(receipt['pid']),
                'scope': 'Original allocation CLI initial capture;actual capture API after host product. Original full FI/axis after-capture CLI branches NOT_RUN.'}
    helpers.write(run/(label+'-observation.json'), evidence)
    return evidence
