"""Admit exact original inlay programs through canonical Source without a server."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'lsp_config_original'))
from generate_subset import function_span

names = ['param_mode_inlay_hints_describe_modes', 'throw_effect_inlay_hints_show_inferred_result']


def generated(original):
    raw = original.read_bytes()
    source = raw.decode('utf-8')
    values = []
    for name in names:
        declaration = re.search(r'^TEST\(' + name + r'\)', source, re.MULTILINE)
        if declaration is None:
            raise ValueError('Missing original inlay test: ' + name)
        start, end = function_span(source, declaration.start())
        match = re.search(r'const char \*content\s*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', source[start:end])
        if match is None:
            raise ValueError('Original source literal structure changed: ' + name)
        values.append(match[1].strip())
    return ('/* Original source SHA256: ' + hashlib.sha256(raw).hexdigest() + ' */\n'
            'static const char *const original_inlay_names[] = {\n' +
            ',\n'.join('"' + name + '"' for name in names) + '\n};\n'
            'static const char *const original_inlay_inputs[] = {\n' + ',\n'.join(values) + '\n};\n')


if sys.argv[1] == '--generate':
    Path(sys.argv[3]).write_text(generated(Path(sys.argv[2])), encoding='utf-8', newline='\n')
else:
    exe, stdlib, original, header = map(Path, sys.argv[1:5])
    if header.read_text(encoding='utf-8') != generated(original).replace('\r\n', '\n'):
        raise SystemExit('Generated original inlay input drifted.')
    with tempfile.TemporaryDirectory(prefix='inlay-source-', dir=exe.resolve().parent) as folder:
        root = Path(folder)
        (root / 'xray.toml').write_text('[project]\nname="inlay-source"\nversion="1.0.0"\nmain="root.xr"\n',
                                       encoding='utf-8', newline='\n')
        entry = root / 'root.xr'
        result = subprocess.run([str(exe.resolve()), entry.as_uri(), sys.argv[5]],
                                env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())), timeout=25)
        if entry.exists():
            raise SystemExit('Source observation mutated the unopened source file.')
        raise SystemExit(result.returncode)
