import json
import pathlib
import subprocess
import sys

exe = pathlib.Path(sys.argv[1]).resolve()
root = exe.parent / (exe.stem + '-cases')
root.mkdir(exist_ok=True)
deep = root
for index in range(12):
    deep /= f'segment{index}_' + 'x' * 80
    deep.mkdir(exist_ok=True)
source = deep / 'source.xr'
source.write_bytes(b'abc')
unicode = deep / '\u4e2d\u6587.xr'
unicode.write_bytes(b'abc')
args = [str(exe), str(root), str(source), str(deep), str(unicode)]
result = subprocess.run(args, cwd=root, capture_output=True)
(exe.parent / (exe.stem + '.stdout')).write_bytes(result.stdout)
(exe.parent / (exe.stem + '.stderr')).write_bytes(result.stderr)
(exe.parent / (exe.stem + '.json')).write_text(json.dumps({
    'args': args, 'returncode': result.returncode, 'path_characters': len(str(source)),
    'fixture_bytes_hex': '616263', 'cwd': str(root),
}, ensure_ascii=False, indent=2), encoding='utf-8')
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
raise SystemExit(result.returncode)
