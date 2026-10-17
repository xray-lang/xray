"""Qualify every complete decimal Source allocation within the original deadline."""
from __future__ import annotations
import time
STARTED = time.monotonic()
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import uuid

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'lib'))
from xraytest.windows_owned_job import establish_job
from xraytest.proc import _terminate_tree

DEADLINE = STARTED + 600
ACTIVE = set()
LOCK = threading.Lock()
CANCELED = threading.Event()
CAPS = [67108864, 8388608, 128000000]


def require(value, message):
    if not value:
        raise RuntimeError(message)


def remaining():
    seconds = DEADLINE - time.monotonic()
    require(seconds > 0, 'Original global 600-second deadline exhausted')
    return seconds


def invoke(exe, folder, label, ordinal=None):
    remaining()
    report = folder / (label + '.json')
    command = [str(exe), '--census', str(report)] if ordinal is None else [str(exe), '--fault', str(ordinal), str(report)]
    with (folder / (label + '.log')).open('wb') as log:
        with LOCK:
            require(not CANCELED.is_set(), 'Qualification already failed')
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            ACTIVE.add(process)
        try:
            code = process.wait(timeout=remaining())
        except BaseException:
            _terminate_tree(process)
            raise
        finally:
            with LOCK:
                ACTIVE.discard(process)
    require(code == 0, f'{label} exited {code}; see {folder / (label + ".log")}')
    require(report.is_file() and report.stat().st_size < 4096, f'Missing report {report}')
    value = json.loads(report.read_text(encoding='utf-8'))
    require(value['caps'] == CAPS and value['status'] == 0 and
            all(value[k] == 0 for k in ['compiler_blocks', 'compiler_bytes', 'runtime_blocks', 'runtime_bytes']),
            f'Changed caps, status or physical owners in {report}')
    require(value['allocated'] <= CAPS[0] and value['peak'] <= CAPS[1] and value['work'] <= CAPS[2], f'Exceeded cap in {report}')
    require(value['fault'] == (ordinal is not None) and
            (ordinal is None or (value['ordinal'] == ordinal and value['same_ledger_retry'])), f'Wrong actual fault/retry {report}')
    require(value['first_status'] == (0 if ordinal is None else 7) and
            (ordinal is None or value['work'] > value['first_work']), f'Wrong first failure or replenished work in {report}')
    remaining()
    return value


def main():
    if os.name == 'nt':
        establish_job()
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    exe = args.exe.resolve()
    folder = args.output.resolve() / uuid.uuid4().hex
    folder.mkdir(parents=True)
    binary = hashlib.sha256(exe.read_bytes()).hexdigest()
    runner = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    census = invoke(exe, folder, 'census')
    sites = census['sites']
    require(isinstance(sites, int) and sites > 0, 'No actual allocation sites')
    invoke(exe, folder, 'strict-last', sites - 1)
    # Every ordinal is retained, including the separately checked final one.
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = [pool.submit(invoke, exe, folder, f'fault-{at:06}', at) for at in range(sites)]
        try:
            for future in concurrent.futures.as_completed(futures, timeout=remaining()):
                future.result()
        except BaseException:
            # Cancel before executor shutdown waits, then close only owned children.
            CANCELED.set()
            for future in futures:
                future.cancel()
            with LOCK:
                processes = list(ACTIVE)
            for process in processes:
                _terminate_tree(process)
            raise
    require(hashlib.sha256(exe.read_bytes()).hexdigest() == binary and
            hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == runner, 'Qualification input changed')
    result = dict(caps=CAPS, sites=sites, covered=sites, workers=8, deadline_seconds=600,
                  same_ledger_retry=True, binary_sha256=binary, runner_sha256=runner,
                  elapsed=time.monotonic() - STARTED, all_physical_zero=True)
    (folder / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except BaseException:
        CANCELED.set()
        with LOCK:
            processes = list(ACTIVE)
        for process in processes:
            _terminate_tree(process)
        raise
