"""Compile receiving-plan C declarations after all producer owners retire."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile


def run(argv, cwd):
    print('$ ' + json.dumps([str(item) for item in argv]), flush=True)
    result = subprocess.run([str(item) for item in argv], cwd=cwd, capture_output=True, timeout=30)
    for data in (result.stdout, result.stderr):
        if data:
            print(data.decode('utf-8', errors='replace'), end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'command exited {result.returncode}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--kind', choices=['msvc', 'gnu'], required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    headers = Path(__file__).resolve().parents[2] / 'include'
    args.work.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='value-types-', dir=args.work)).resolve()
    source = work / 'values.c'
    product = work / ('values.exe' if os.name == 'nt' else 'values')
    run([args.driver.resolve(), source], work)
    text = source.read_text(encoding='ascii')
    assert 'XrtManagedHeader *' in text and 'field offset' in text and 'nullable payload' in text
    assert all(word not in text for word in ('Xir', 'XrtBoxed', '../src', '({'))
    if args.kind == 'msvc':
        command = [args.compiler, '/nologo', '/std:c11', '/W4', '/WX', '/utf-8', '/I' + str(headers), source,
                   '/Fe' + str(product), '/Fo' + str(work / 'values.obj')]
    else:
        command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic', '-I' + str(headers),
                   source, '-o', product]
    run(command, work)
    run([product], work)
    print('receiving-plan typed C compiled and executed after all producer retirement', flush=True)


if __name__ == '__main__':
    main()
