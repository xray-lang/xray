"""Exercise real static-library clients; retain bytes and negative link logs."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--suite', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--clients', nargs='+', required=True)
    parser.add_argument('--logs', type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)

    def run(label, client=0, mode='write-error', location=1, pipe=False):
        command = [args.clients[client], mode, str(location)]
        if pipe:
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                process = subprocess.run(command, stdout=write_fd, stderr=subprocess.PIPE, timeout=15)
            finally:
                os.close(write_fd)
            output = b''
        else:
            process = subprocess.run(command, capture_output=True, timeout=15)
            output = process.stdout
        (args.logs / (label + '.stdout')).write_bytes(output)
        (args.logs / (label + '.stderr')).write_bytes(process.stderr)
        (args.logs / (label + '.exit')).write_text(str(process.returncode))
        assert b'AFTER_CALL' not in process.stderr and b'EXIT_CALLBACK' not in process.stderr, label
        return process.returncode, output, process.stderr

    if args.suite == 'output':
        code, output, error = run('raw-values', mode='values')
        assert (code, output, error) == (0, b'-9223372036854775808\n0\n9223372036854775807\n', b'')
        assert run('prefix-order', mode='prefix') == (0, b'prefix:12\n', b'')
    elif args.suite == 'failures':
        cases = [
            ('location', 0, 1, b'source\\x0a.xrc:3:5-7:9'),
            ('unknown', 0, 0, b'unknown source'),
            ('empty', 1, 0, b'unknown source'),
            ('empty-id', 1, 1, b'invalid source id 1'),
            ('range', 0, 4294967295, b'invalid source id 4294967295'),
            ('start-only', 7, 1, b'start.xrc:3:5 ['),
        ]
        for label, client, location, expected in cases:
            code, output, error = run(label, client, location=location)
            assert code == 74 and not output, (label, code, output, error)
            assert error.startswith(b'xray: output write failed at ') and expected in error, (label, error)
            assert error.count(b'\n') == 1 and b'!' not in error, (label, error)
            assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), (label, error)
        for client in [2, 3, 4, 5, 6, 8, 9]:
            code, output, error = run('invalid-root-' + str(client), client, mode='values')
            assert code != 0 and not output, (client, code, output, error)
            assert b'valid prepared source map' in error, (client, error)
        code, output, error = run('real-flush', mode='flush-error')
        assert code == 74 and not output and b'output flush failed at ' in error, (code, output, error)
        assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), error
        code, output, error = run('stale-codes', mode='stale-flush-error')
        assert code == 74 and not output and b'output flush failed at ' in error, (code, output, error)
        assert re.search(rb'\[(?:win32|errno)=[1-9][0-9]*\]\n$', error), error
        code, output, error = run('real-broken-pipe', mode='pipe', pipe=True)
        assert (code, output, error) == (74, b'', b''), (code, output, error)
        code, output, error = run('real-buffered-broken-pipe', mode='pipe-flush', pipe=True)
        assert (code, output, error) == (74, b'', b''), (code, output, error)
    elif args.suite == 'link_contract':
        for name, diagnostic in [('missing', rb'LNK20(?:01|19)|undefined (?:reference|symbol)'),
                                 ('duplicate', rb'LNK2005|multiple definition|duplicate symbol')]:
            result = subprocess.run(['cmake', '--build', args.build, '--target',
                                     'test_native_runtime_' + name, '-j1'],
                                    capture_output=True, timeout=60)
            combined = result.stdout + result.stderr
            (args.logs / (name + '.log')).write_bytes(combined)
            assert result.returncode and b'xrt_standalone_source_map' in combined, combined
            assert re.search(diagnostic, combined, re.IGNORECASE), combined
    else:
        raise AssertionError(args.suite)
    print('PASS:', args.suite)


if __name__ == '__main__':
    main()
