"""Verify real VM output and host survival with the production runtime."""
import argparse
import datetime
import os
from pathlib import Path
import subprocess
import time



def check_offsets(output, runs):
    fields = output.splitlines()
    assert len(fields) == runs * 6
    def host_offset(seconds):
        local = datetime.datetime(*time.localtime(seconds)[:6])
        utc = datetime.datetime(*time.gmtime(seconds)[:6])
        return int((local - utc).total_seconds() / 60)
    for run in range(runs):
        fixed = [int(x) for x in fields[run * 6:run * 6 + 6]]
        assert fixed[:3] == [host_offset(t) for t in (0, 1700000000, 1719792000)]
        assert fixed[4] in (host_offset(fixed[3]), host_offset(fixed[5]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True)
    parser.add_argument('--logs', required=True, type=Path)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)

    def run(mode, order='program-first', pipe=False):
        command = [args.client, mode, order]
        if pipe:
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                result = subprocess.run(command, stdout=write_fd, stderr=subprocess.PIPE, timeout=20)
            finally:
                os.close(write_fd)
            output = b''
        else:
            result = subprocess.run(command, capture_output=True, timeout=20)
            output = result.stdout
        label = mode + '-' + order
        (args.logs / (label + '.stdout')).write_bytes(output)
        (args.logs / (label + '.stderr')).write_bytes(result.stderr)
        (args.logs / (label + '.exit')).write_text(str(result.returncode))
        if mode.startswith('panic-'):
            message = b'modulo' if mode == 'panic-rem' else b'division'
            code = b'421' if mode == 'panic-rem' else b'420'
            expected = b'' if mode == 'panic-bad-sink' else (
                b'[Uncaught Panic] E0' + code + b': ' + message + b' by zero\n')
            assert result.returncode == 1 and not output and result.stderr == expected, (
                label, result.returncode, output, result.stderr)
            return
        assert result.returncode == 0, (label, result.returncode, output, result.stderr)
        if mode == 'offset-source':
            check_offsets(output, 2)
            assert not result.stderr
        elif mode in ('source', 'literal-repeat', 'string-repeat', 'clock-source'):
            expected = (b'1\n1\n' if mode == 'clock-source' else b'A\x00\xc3\xa9\n7\n\n'*2 if mode == 'string-repeat' else
                        b'7\n99\n7\n99\n' if mode == 'literal-repeat' else b'10\n21\n')
            assert output == expected and result.stderr == b'', (output, result.stderr)
        else:
            marker = b'HOST_ALIVE\r\n' if os.name == 'nt' else b'HOST_ALIVE\n'
            assert not output and result.stderr == marker, (label, output, result.stderr)

    run('source')
    run('clock-source')
    run('offset-source')
    run('literal-repeat')
    run('string-repeat')
    for mode in ('write', 'flush', 'pipe', 'pipe-flush', 'point', 'unknown', 'source-failure', 'literal-failure'):
        for order in ('program-first', 'outcome-first'):
            run(mode, order, pipe=mode in ('pipe', 'pipe-flush'))
    for mode in ('string-write', 'string-flush', 'string-pipe', 'string-pipe-flush',
                 'string-point', 'string-unknown', 'string-source-failure'):
        for order in ('program-first', 'outcome-first'):
            run(mode, order, pipe=mode in ('string-pipe', 'string-pipe-flush'))
    run('nested')
    run('nested-string')
    for mode in ('panic-div', 'panic-rem', 'panic-bad-sink'):
        for order in ('program-first', 'outcome-first'):
            run(mode, order)
    print('VM output, owned failure lifetime and host continuation passed')


if __name__ == '__main__':
    main()
