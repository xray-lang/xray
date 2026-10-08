#!/usr/bin/env python3
"""Exercise real process receipts with normal, nonzero and forced termination."""
from argparse import ArgumentParser
from datetime import datetime, timezone
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import time
import uuid


def windows_terminal(pid):
    if os.name != 'nt':
        return {'platform': os.name, 'independent_windows_observation': 'NOT_RUN'}
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    handle = kernel.OpenProcess(0x00100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error != 87:
            raise RuntimeError(f'PID {pid} terminal state is not observable: WinError {error}')
        return {'platform': 'Windows', 'pid': pid, 'observation': 'PID_ABSENT', 'winerror': error}
    try:
        wait = kernel.WaitForSingleObject(handle, 0)
        if wait != 0:
            raise RuntimeError(f'PID {pid} remains live or wait failed: {wait}')
        return {'platform': 'Windows', 'pid': pid, 'observation': 'HANDLE_SIGNALED', 'wait_result': wait}
    finally:
        if not kernel.CloseHandle(handle):
            raise RuntimeError(f'PID {pid} observation handle close failed')


def main():
    parser = ArgumentParser(description=__doc__)
    for name in ('binary', 'root', 'stdlib', 'input-root', 'source-file', 'registration-file', 'evidence-root'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    for name in ('binary', 'root', 'stdlib', 'input_root', 'source_file', 'registration_file'):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.file = args.root / 'root.xr'
    run = args.evidence_root.resolve() / ('process-lifecycle-' + uuid.uuid4().hex)
    run.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('allocation_faults', args.input_root/'scripts/source_product_consumer_allocation_faults.py')
    fi = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fi)
    helpers = fi.load_helpers(args.input_root)
    axes_spec = importlib.util.spec_from_file_location('allocation_axes', args.input_root/'scripts/source_product_consumer_allocation_axes.py')
    axes = importlib.util.module_from_spec(axes_spec)
    axes_spec.loader.exec_module(axes)
    before = fi.capture_inputs(args, helpers)
    helpers.write(run/'inputs-before.json', before)
    report = {'status': 'RUNNING', 'processes': [], 'issues': [], 'evidence': str(run),
              'scope': 'REAL_PROCESS_LIFECYCLE', 'full_FI': 'NOT_RUN', 'axes': 'NOT_RUN',
              'reap_error': 'NOT_RUN', 'compiler_cleanup_after_kill': 'NOT_PROVEN',
              'Main': 'NOT_RUN', 'retired': 0}
    helpers.write(run/'result.json', report)
    for helper_name, mode in ((name, mode) for name in ('faults', 'axes') for mode in ('normal', 'nonzero', 'timeout')):
        runner = fi if helper_name == 'faults' else axes
        command = [str(args.binary), str(args.root), str(args.stdlib), mode]
        label = helper_name + '-' + mode
        row = {'helper': helper_name, 'mode': mode, 'status': 'RUNNING'}
        report['processes'].append(row)
        try:
            budget = 3 if mode == 'timeout' else 20
            receipt = runner.process_run(command, label, run, time.monotonic() + budget, helpers, args.input_root)
            row['receipt'] = receipt
            if (receipt['budget_clock_origin'] != 'process_run entry' or receipt['reap_grace_seconds'] != 8
                    or not 0 < receipt['wait_budget_seconds'] <= budget
                    or abs(receipt['reap_deadline_seconds'] - receipt['wait_budget_seconds'] - 8) > 0.000001):
                raise ValueError('receipt does not describe the actual invocation deadline and reaping grace')
            out = Path(receipt['stdout']).read_text(encoding='utf8')
            err = Path(receipt['stderr']).read_text(encoding='utf8')
            for stream in ('stdout', 'stderr'):
                if hashlib.sha256(Path(receipt[stream]).read_bytes()).hexdigest() != receipt[stream+'_sha256']:
                    raise ValueError('raw output hash differs from receipt')
            if not receipt['entered'] or not receipt['pid'] or not receipt['termination_observed'] or receipt['termination_errors'] or receipt['launch_error']:
                raise ValueError('actual launch or bounded process reaping was not proved')
            ready = re.fullmatch(r'process-lifecycle ready=1 mode=' + mode + r' product-verified=1 session-dead=1 physical=([1-9][0-9]*)/([1-9][0-9]*)', out.splitlines()[0])
            if not ready or err != 'process-lifecycle stderr-ready mode=' + mode + '\n':
                raise ValueError('real product readiness or stderr preservation missing')
            row['ready_physical'] = [int(v) for v in ready.groups()]
            if mode == 'timeout':
                if not receipt['timed_out'] or not receipt['execution_error'] or receipt['returncode'] in (None, 0, 29):
                    raise ValueError('deliberate wait was not actually terminated by the timeout path')
                if len(out.splitlines()) != 1 or 'released=1' in out:
                    raise ValueError('forced termination unexpectedly claimed normal cleanup')
            else:
                if receipt['timed_out'] or receipt['execution_error'] or receipt['returncode'] != (23 if mode == 'nonzero' else 0):
                    raise ValueError('ordinary process status differs')
                if len(out.splitlines()) != 3 or 'compiler physical blocks/bytes=0/0; peak=' not in out or f'process-lifecycle released=1 mode={mode} physical=0/0' not in out:
                    raise ValueError('ordinary owner destruction did not complete')
            try:
                runner.process_contract(receipt)
                row['process_contract'] = 'PASS'
            except ValueError as error:
                row['process_contract'] = 'FAIL'
                row['preserved_failure'] = str(error)
            if row['process_contract'] != ('PASS' if mode == 'normal' else 'FAIL'):
                raise ValueError('runner accepted an abnormal process as success')
            row['terminal_observation'] = windows_terminal(receipt['pid'])
            row['status'] = 'PASS'
        except Exception as error:
            row['status'] = 'FAIL'
            row['error'] = repr(error)
            row['failure_receipt'] = fi.process_failure(command, label, run, error, helpers)
            report['issues'].append(label + ': ' + repr(error))
        helpers.write(run/'result.json', report)
    if os.name == 'nt':
        reap_spec = importlib.util.spec_from_file_location('native_reap_error', args.input_root/'scripts/source_product_consumer_process_reap_error.py')
        reap_probe = importlib.util.module_from_spec(reap_spec)
        reap_spec.loader.exec_module(reap_probe)
        for helper_name, runner in (('faults', fi), ('axes', axes)):
            label = helper_name + '-reap-error'
            command = [str(args.binary), str(args.root), str(args.stdlib), 'timeout']
            row = {'helper': helper_name, 'mode': 'reap-error', 'status': 'RUNNING'}
            report['processes'].append(row)
            try:
                row.update(reap_probe.exercise(runner, helpers, command, label, run, args.input_root))
                row['terminal_observation'] = windows_terminal(row['receipt']['pid'])
                row['status'] = 'PASS'
            except Exception as error:
                row['status'] = 'FAIL'
                row['error'] = repr(error)
                row['failure_receipt'] = fi.process_failure(command, label, run, error, helpers)
                report['issues'].append(label + ': ' + repr(error))
            helpers.write(run/'result.json', report)
        report['reap_error'] = 'PASS_NATIVE_INVALID_HANDLE_FAULT' if all(x['status']=='PASS' for x in report['processes'] if x['mode']=='reap-error') else 'FAIL'
        write_spec = importlib.util.spec_from_file_location('native_write_error', args.input_root/'scripts/source_product_consumer_process_write_error.py')
        write_probe = importlib.util.module_from_spec(write_spec)
        write_spec.loader.exec_module(write_probe)
        for helper_name, phase in ((h, p) for h in ('faults', 'axes') for p in ('pid-write', 'final-write')):
            runner = fi if helper_name == 'faults' else axes
            label = helper_name + '-' + phase
            command = [str(args.binary), str(args.root), str(args.stdlib), 'timeout' if phase=='pid-write' else 'normal']
            row = {'helper': helper_name, 'mode': phase, 'status': 'RUNNING'}
            report['processes'].append(row)
            try:
                row.update(write_probe.exercise(runner, fi, helpers, command, label, phase, run, args.input_root, windows_terminal))
                row['status'] = 'PASS'
            except Exception as error:
                row['status'] = 'FAIL'
                row['error'] = repr(error)
                row['failure_receipt'] = fi.process_failure(command, label, run, error, helpers)
                report['issues'].append(label + ': ' + repr(error))
            helpers.write(run/'result.json', report)
        report['receipt_write_error'] = 'PASS_NATIVE_SHARING_FAULT' if all(x['status']=='PASS' for x in report['processes'] if x['mode'] in ('pid-write','final-write')) else 'FAIL'
    try:
        after = fi.capture_inputs(args, helpers)
        helpers.write(run/'inputs-after.json', after)
        if before != after:
            raise ValueError('actual captured SHA,size,mtime or capture origin changed')
    except Exception as error:
        report['issues'].append('input preservation: ' + repr(error))
    report.update(status='FAIL' if report['issues'] else 'PASS', inputs=len(before['files']),
                  finished_at=datetime.now(timezone.utc).isoformat(), parent_pid=os.getpid(),
                  boundary='Host process runner qualification only. Timeout children hold verified public SourceProducts. Native Windows handle-error injection closes one child handle,retains first failures,and uses a separate owned handle for recovery. Kill/reap does not prove product cleanup,FI coverage or sanitizer shutdown. Other OS error causes remain unverified.')
    helpers.write(run/'result.json', report)
    print(json.dumps(report, ensure_ascii=False))
    if report['status'] != 'PASS':
        raise SystemExit(1)


if __name__ == '__main__':
    main()
