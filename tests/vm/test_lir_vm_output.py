"""Verify real VM output and host survival with the production runtime."""
import argparse
import os
from pathlib import Path
import subprocess


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
        assert result.returncode == 0, (label, result.returncode, output, result.stderr)
        if mode == 'source':
            assert output == b'10\n21\n' and result.stderr == b'', (output, result.stderr)
        else:
            marker = b'HOST_ALIVE\r\n' if os.name == 'nt' else b'HOST_ALIVE\n'
            assert not output and result.stderr == marker, (label, output, result.stderr)

    run('source')
    for mode in ('write', 'flush', 'pipe', 'pipe-flush', 'point', 'unknown', 'source-failure'):
        for order in ('program-first', 'outcome-first'):
            run(mode, order, pipe=mode in ('pipe', 'pipe-flush'))
    run('nested')
    print('VM output, owned failure lifetime and host continuation passed')


if __name__ == '__main__':
    main()
