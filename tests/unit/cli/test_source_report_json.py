"""The real test command preserves the complete owned diagnostic report in JSON."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--xray', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    directory = output/'long'
    index = 0
    while len(str(directory/'two-independent-items.xr')) <= 512:
        directory = directory/(str(index)+'_'+'x'*78)
        index += 1
    directory.mkdir(parents=True)
    source = directory/'two-independent-items.xr'
    source.write_bytes(b'const alpha:i64=1\nconst alpha:i64=2\nconst beta:i64=3\nconst beta:i64=4\nprint(9)\n')
    assert len(str(source)) > 512
    report = output/'report.json'
    command = [str(args.xray.resolve()), 'test', str(source), '--report', str(report)]
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(args.root.resolve()/'stdlib'), NO_COLOR='1')
    child = subprocess.run(command, env=environment, capture_output=True, timeout=90)
    (output/'stdout').write_bytes(child.stdout)
    (output/'stderr').write_bytes(child.stderr)
    assert child.returncode == 1, (child.returncode, child.stdout, child.stderr)
    data = json.loads(report.read_text(encoding='utf-8'))
    assert len(data['files']) == 1
    result = data['files'][0]
    assert result['errors'] == 1 and result['sourceStatus'] == 'source-rejected'
    assert result['sourceStage'] == 4
    error = result['error'].replace('\\', '/')
    path = str(source).replace('\\', '/')
    expected = ''.join(f'{path}:{line}:7: {kind}: {message}\n' for line, kind, message in (
        (2, 'error', 'duplicate declaration name'), (1, 'note', 'first declaration is here'),
        (4, 'error', 'duplicate declaration name'), (3, 'note', 'first declaration is here')))
    assert len(error) > 512 and error == expected, (len(error), error, expected)
    assert source.resolve().is_relative_to(output)
    source.unlink()  # The producer process closed its source handle and owned report.
    assert json.loads(report.read_text(encoding='utf-8'))['files'][0]['error'] == result['error']
    summary = dict(status='PASS', command=command, executable_sha256=hashlib.sha256(args.xray.read_bytes()).hexdigest(),
                   path_length=len(str(source)), error_length=len(error), roots=2, labels=2,
                   source_removed=True, report_sha256=hashlib.sha256(report.read_bytes()).hexdigest(), output=str(output))
    (output/'result.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
